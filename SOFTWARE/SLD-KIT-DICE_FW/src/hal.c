// SPDX-License-Identifier: MIT
// Copyright (c) 2026 OLIMEX LTD BULGARIA
//
// SLD-KIT-DICE Rev.A - hardware layer: LED scanning, speaker, shock sensor,
// standby. Pin map and LED layout: see hal.h.
//
// Overview
//   SysTick   1 ms tick: counts hal_ms, scans the LEDs, times the tones and
//             reads the shock sensor. It is the only interrupt while awake.
//   TIM1      speaker PWM on PC4 (TIM1_CH4).
//   TIM2      free-running 24 MHz counter that captures the sensor's edges.
//   EXTI2     wakes the chip from standby when the sensor closes.

#include "hal.h"

#define TICKS_PER_MS (FUNCONF_SYSTEM_CORE_CLOCK / 8000)  // SysTick = HCLK/8 = 3 MHz

#define TIM_OC4M_MASK  (TIM_OC4M_2 | TIM_OC4M_1 | TIM_OC4M_0)
#define OC4M_PWM1      (TIM_OC4M_2 | TIM_OC4M_1)   // 110: PWM mode 1
#define OC4M_FORCE_LOW (TIM_OC4M_2)                 // 100: forced inactive

volatile uint32_t hal_ms;
volatile uint8_t  hal_leds;
volatile uint32_t hal_shock_count;
volatile uint32_t hal_shock_ms;
volatile uint32_t hal_shock_events;
uint32_t hal_reset_flags;
uint32_t hal_last_phase;
uint32_t hal_boot_mcause;
hal_crash_t hal_last_crash;

// Shock sensor while awake: TIM2 free-runs at 24 MHz and captures the time
// of the last closing (falling edge, CH2) and opening (rising edge, CH1), both
// on PC2 via TIM2 partial remap 1. SysTick reads the captures every ms, so
// the sensor never interrupts the CPU, however fast the spring chatters.
// (An interrupt per edge was tried first: a hard shake gives thousands of
// edges per second and starved the program.)
// The edge times come from the spring, not from our clock, so they are real
// entropy: when each closing happened, the time between closings and how
// long the contact lasted.
// EXTI on PC2 is armed only in standby, to wake the chip.
//
// SHOCK_FILTER = TIM IC2F. 0 = none; 1..3 = 2/4/8 samples at 24 MHz
// (83/167/333 ns). It rejects only glitches: the sensor gives real pulses
// shorter than 10 us, which a stronger filter would lose.
#define SHOCK_FILTER 0x3

static volatile uint8_t  swio_released;
static uint16_t shock_last_fall;      // TIM2 time of the previous closing
static volatile uint16_t tone_left;   // ms left, 0xFFFF = until stopped
static volatile uint16_t led_on_ticks = TICKS_PER_MS;  // LED on-time in each 1 ms slot
static uint8_t tick_off_pending;      // SysTick: next compare is "LEDs off"
static uint16_t led_on_cur;           // led_on_ticks as used in the current slot
volatile uint8_t hal_tone_duty = 50;
static volatile uint32_t entropy;

// ---------------------------------------------------------------------------
// GPIO helpers. Each pin has a 4-bit field in CFGLR (mode + configuration).
// CFGLR of GPIOA and of PC1 is changed only by the scanner (in the SysTick
// interrupt) and by hal_sleep_until_shock() with SysTick off, so these
// read-modify-write sequences never race.

static inline void pin_cfg(GPIO_TypeDef *port, int n, uint32_t mode)
{
	port->CFGLR = (port->CFGLR & ~(0xFu << (4 * n))) | (mode << (4 * n));
}

// Drive a pin: set the level first, then switch to output, so it never
// glitches to the wrong level.
static inline void pin_out(GPIO_TypeDef *port, int n, int high)
{
	port->BSHR = high ? (1u << n) : (1u << (n + 16));
	pin_cfg(port, n, GPIO_CFGLR_OUT_2Mhz_PP);
}

// All LEDs dark: the 3 charlieplex lines float, PD1 low.
static inline void lines_off(void)
{
	pin_cfg(GPIOA, 1, GPIO_CFGLR_IN_FLOAT);
	pin_cfg(GPIOA, 2, GPIO_CFGLR_IN_FLOAT);
	pin_cfg(GPIOC, 1, GPIO_CFGLR_IN_FLOAT);
	if (swio_released)
		GPIOD->BCR = 1u << 1;
}

// ---------------------------------------------------------------------------
// LED scanner, called every millisecond from SysTick.
//
// 4 slots, one per millisecond: in each, one line is driven high and the
// LEDs wanted from it get their cathode line pulled low. Each LED is thus
// lit 1/4 of the time, refreshed at 250 Hz.
// Within a slot the LEDs are on only for led_on_ticks (hal_led_level): SysTick
// fires a second time and turns them off. That dims them and saves current.
// At most two LEDs are lit at once (~2 x 11 mA), and none while a tone plays.
static void scan(void)
{
	static uint8_t slot;
	uint8_t m = hal_leds;

	lines_off();

	// LEDs and speaker never draw current at the same time: a CR2032 can't
	// deliver both without its voltage sagging.
	if (tone_left) {
		slot = (slot + 1) & 3;
		return;
	}

	switch (slot) {
	case 0: // PA1 high: LED1 (to PA2), LED3 (to PC1)
		if (m & (LED1 | LED3)) {
			if (m & LED1) pin_out(GPIOA, 2, 0);
			if (m & LED3) pin_out(GPIOC, 1, 0);
			pin_out(GPIOA, 1, 1);
		}
		break;
	case 1: // PA2 high: LED2 (to PA1), LED5 (to PC1)
		if (m & (LED2 | LED5)) {
			if (m & LED2) pin_out(GPIOA, 1, 0);
			if (m & LED5) pin_out(GPIOC, 1, 0);
			pin_out(GPIOA, 2, 1);
		}
		break;
	case 2: // PC1 high: LED4 (to PA1), LED6 (to PA2)
		if (m & (LED4 | LED6)) {
			if (m & LED4) pin_out(GPIOA, 1, 0);
			if (m & LED6) pin_out(GPIOA, 2, 0);
			pin_out(GPIOC, 1, 1);
		}
		break;
	default: // PD1 high: LED7 (not charlieplexed, has its own pin)
		if (swio_released && (m & LED7))
			GPIOD->BSHR = 1u << 1;
		break;
	}
	slot = (slot + 1) & 3;
}

// ---------------------------------------------------------------------------
// Interrupts

// SysTick counts up continuously; each interrupt moves the compare value on.
// With LEDs dimmed there are two interrupts per millisecond:
//   1. at the start of the slot: LEDs on, next compare after led_on_ticks;
//   2. after led_on_ticks: LEDs off, next compare at the start of the next slot.
void SysTick_Handler(void) __attribute__((interrupt));
void SysTick_Handler(void)
{
	SysTick->SR = 0;

	if (tick_off_pending) {            // 2. end of the LED on-time in this slot
		tick_off_pending = 0;
		lines_off();
		SysTick->CMP += TICKS_PER_MS - led_on_cur;
		return;
	}

	uint16_t on = led_on_cur = led_on_ticks;   // 1. start of a slot
	if (on < TICKS_PER_MS) {
		SysTick->CMP += on;
		tick_off_pending = 1;
	} else {
		SysTick->CMP += TICKS_PER_MS;          // full brightness: one interrupt
	}
	hal_ms++;

	scan();

	// Tone timing: stop the PWM when the time is up.
	if (tone_left && tone_left != 0xFFFF && --tone_left == 0) {
		TIM1->CHCTLR2 = (TIM1->CHCTLR2 & ~TIM_OC4M_MASK) | OC4M_FORCE_LOW;
		TIM1->CTLR1 &= ~TIM_CEN;
	}

	// Shock sensor captures (see the top of this file). Every value read
	// here is folded into the entropy pool; most of its randomness is in
	// the low bits of the 24 MHz capture times.
	uint16_t sr = TIM2->INTFR;
	if (sr & (TIM_CC1IF | TIM_CC2IF)) {
		uint32_t x = sr ^ (hal_ms << 16);
		if (sr & TIM_CC2IF) {                      // closed at least once
			uint16_t fall = TIM2->CH2CVR;           // reading clears CC2IF
			x ^= (uint32_t)fall << 8;
			x ^= (uint16_t)(fall - shock_last_fall); // time since previous closing
			shock_last_fall = fall;
			hal_shock_count += (sr & TIM_CC2OF) ? 2 : 1;  // OF = more than one
			hal_shock_events++;
			hal_shock_ms = hal_ms;
		}
		if (sr & TIM_CC1IF) {                      // opened at least once
			uint16_t rise = TIM2->CH1CVR;
			x ^= (uint32_t)(uint16_t)(rise - shock_last_fall) << 4;  // contact time
			x ^= (uint32_t)rise << 16;
		}
		if (sr & (TIM_CC1OF | TIM_CC2OF))
			TIM2->INTFR = (uint16_t)~(TIM_CC1OF | TIM_CC2OF);  // write 0 to clear
		entropy = ((entropy << 5) | (entropy >> 27)) ^ x;      // rotate and mix
		entropy += 0x9E3779B9u;
	}
}

void EXTI7_0_IRQHandler(void) __attribute__((interrupt));
void EXTI7_0_IRQHandler(void)
{
	// Only armed in standby: one pulse wakes us, then TIM2 takes over.
	EXTI->INTENR &= ~EXTI_Line2;
	EXTI->INTFR = EXTI_Line2;
	hal_shock_count++;
	hal_shock_ms = hal_ms;
}

// Faults: save what happened and reset, instead of hanging forever.
// The record survives the reset (see hal.h) and can be read with minichlink.
static void __attribute__((noreturn)) crash(void)
{
	volatile hal_crash_t *c = HAL_CRASH;
	if (c->magic != HAL_CRASH_MAGIC)
		c->count = 0;
	c->mcause = __get_MCAUSE();
	c->mepc = __get_MEPC();
	c->mtval = __get_MTVAL();
	c->intsyscr = __get_INTSYSCR();
	c->count++;
	c->magic = HAL_CRASH_MAGIC;
	NVIC_SystemReset();
	for (;;) ;
}

void HardFault_Handler(void) __attribute__((interrupt));
void HardFault_Handler(void) { crash(); }

void NMI_Handler(void) __attribute__((interrupt));
void NMI_Handler(void) { crash(); }

// ---------------------------------------------------------------------------
// Initialisation

static void systick_start(void)
{
	tick_off_pending = 0;
	SysTick->CTLR = 0;
	SysTick->SR = 0;
	SysTick->CMP = SysTick->CNT + TICKS_PER_MS;
	SysTick->CTLR = SYSTICK_CTLR_STE | SYSTICK_CTLR_STIE;  // HCLK/8, free running
	NVIC_EnableIRQ(SysTicK_IRQn);
}

static void speaker_pin_timer(void)
{
	pin_cfg(GPIOC, 4, GPIO_CFGLR_OUT_10Mhz_AF_PP);  // PC4 driven by TIM1_CH4
}

void hal_init(void)
{
	// Why did we start? Read this before anything else changes it.
	hal_boot_mcause = __get_MCAUSE();
	hal_reset_flags = RCC->RSTSCKR;
	RCC->RSTSCKR |= RCC_RMVF;  // clear, so the next reset shows only its own cause

	// RAM kept across resets (hal.h). After a power-on reset the crash record
	// and the phase may be random, so they are ignored.
	if (hal_reset_flags & RCC_PORRSTF)
		HAL_CRASH->magic = 0;
	if (HAL_CRASH->magic == HAL_CRASH_MAGIC)
		hal_last_crash = *(hal_crash_t *)HAL_CRASH;
	if (!(hal_reset_flags & RCC_PORRSTF) && (HAL_PHASE & 0xFFFF0000u) == HAL_PHASE_MAGIC)
		hal_last_phase = HAL_PHASE & 0xFF;
	HAL_PHASE = 0;

	// ch32fun uses 0 flash wait states at 24 MHz, which is the datasheet limit.
	// One wait state gives margin against bad instruction fetches when the
	// supply is low.
	FLASH->ACTLR = FLASH_ACTLR_LATENCY_1;

	funGpioInitAll();  // GPIOA/C/D + AFIO clocks
	RCC->APB2PCENR |= RCC_APB2Periph_TIM1;
	RCC->APB1PCENR |= RCC_APB1Periph_PWR;

	// Pads not bonded on SOP-8: pull-down, so they don't float and draw
	// current in standby. PD4/PD5 (bonded to PD1) and PD6 (bonded to PA1)
	// stay floating inputs (reset default), so they never fight the pins
	// they share.
	pin_cfg(GPIOA, 0, GPIO_CFGLR_IN_PUPD);
	GPIOA->BCR = 1u << 0;
	for (int n = 0; n < 8; n++) {
		if (n == 1 || n == 2 || n == 4) continue;  // PC1, PC2, PC4 are used
		pin_cfg(GPIOC, n, GPIO_CFGLR_IN_PUPD);
		GPIOC->BCR = 1u << n;
	}
	pin_cfg(GPIOD, 0, GPIO_CFGLR_IN_PUPD);
	pin_cfg(GPIOD, 2, GPIO_CFGLR_IN_PUPD);
	pin_cfg(GPIOD, 3, GPIO_CFGLR_IN_PUPD);
	GPIOD->BCR = (1u << 0) | (1u << 2) | (1u << 3);

	// LED lines off. PD1 stays SWIO until hal_release_swio().
	lines_off();

	// Shock sensor: PC2 input with pull-up; the sensor closes to GND.
	pin_cfg(GPIOC, 2, GPIO_CFGLR_IN_PUPD);
	GPIOC->BSHR = 1u << 2;

	// EXTI line 2 = port C, falling edge. Armed only in hal_sleep_until_shock().
	AFIO->EXTICR = (AFIO->EXTICR & ~(3u << (2 * 2))) | (2u << (2 * 2));
	EXTI->FTENR |= EXTI_Line2;
	EXTI->INTFR = EXTI_Line2;
	NVIC_EnableIRQ(EXTI7_0_IRQn);

	// TIM2: free-running 24 MHz, input captures on TI2 (PC2, partial remap 1).
	// CH2 captures falling edges (TI2 direct), CH1 rising edges (TI2 indirect).
	// No TIM2 outputs are enabled, so the other remapped pins are not affected.
	RCC->APB1PCENR |= RCC_APB1Periph_TIM2;
	RCC->APB1PRSTR |= RCC_APB1Periph_TIM2;
	RCC->APB1PRSTR &= ~RCC_APB1Periph_TIM2;
	AFIO->PCFR1 = (AFIO->PCFR1 & ~AFIO_PCFR1_TIM2_REMAP) | AFIO_PCFR1_TIM2_REMAP_PARTIALREMAP1;
	TIM2->PSC = 0;
	TIM2->ATRLR = 0xFFFF;
	TIM2->CHCTLR1 = TIM_CC1S_1                          // CC1 = input TI2 (indirect)
	              | TIM_CC2S_0 | (SHOCK_FILTER << 12);  // CC2 = input TI2, IC2F filter
	TIM2->CCER = TIM_CC1E                               // CH1: rising edge
	           | TIM_CC2E | TIM_CC2P;                   // CH2: falling edge
	TIM2->CTLR1 = TIM_CEN;

	// Speaker: TIM1_CH4 on PC4, output held low (coil off) until a tone plays.
	RCC->APB2PRSTR |= RCC_APB2Periph_TIM1;
	RCC->APB2PRSTR &= ~RCC_APB2Periph_TIM1;
	TIM1->PSC = 0;
	TIM1->CHCTLR2 = OC4M_FORCE_LOW;
	TIM1->CCER = TIM_CC4E;
	TIM1->BDTR = TIM_MOE;
	GPIOC->BCR = 1u << 4;
	speaker_pin_timer();

	systick_start();
}

// ---------------------------------------------------------------------------
// PD1: programming pin and LED7
//
// The programmer talks to the chip through SWIO (PD1). LED7 hangs on the same
// pin, so the program keeps SWIO on for a few seconds after power-up (time to
// program the chip) and only then turns PD1 into a normal output.

void hal_release_swio(void)
{
	AFIO->PCFR1 = (AFIO->PCFR1 & ~AFIO_PCFR1_SWCFG) | AFIO_PCFR1_SWCFG_DISABLE;
	GPIOD->BCR = 1u << 1;
	pin_cfg(GPIOD, 1, GPIO_CFGLR_OUT_2Mhz_PP);
	swio_released = 1;  // set last: the scanner starts using PD1 only now
}

int hal_swio_released(void)
{
	return swio_released;
}

// ---------------------------------------------------------------------------
// Speaker
//
// TIM1 counts at 24 MHz; the period (ATRLR) sets the pitch and the compare
// value (CH4CVR) the duty, i.e. the volume. The coil is a heavy load for a
// coin cell, so a lower duty is both quieter and gentler on the battery.

void hal_tone(uint16_t hz, uint16_t ms)
{
	if (hz < 400) hz = 400;  // keeps ATRLR within 16 bits at 24 MHz
	uint32_t arr = FUNCONF_SYSTEM_CORE_CLOCK / hz - 1;

	__disable_irq();
	lines_off();  // LEDs dark before the speaker starts (scanner keeps them dark)
	TIM1->CTLR1 &= ~TIM_CEN;
	TIM1->ATRLR = arr;
	TIM1->CH4CVR = (arr + 1) * hal_tone_duty / 100;
	TIM1->CNT = 0;
	TIM1->CHCTLR2 = (TIM1->CHCTLR2 & ~TIM_OC4M_MASK) | OC4M_PWM1;
	TIM1->CTLR1 |= TIM_CEN;
	tone_left = ms ? ms : 0xFFFF;
	__enable_irq();
}

void hal_tone_off(void)
{
	__disable_irq();
	TIM1->CHCTLR2 = (TIM1->CHCTLR2 & ~TIM_OC4M_MASK) | OC4M_FORCE_LOW;
	TIM1->CTLR1 &= ~TIM_CEN;
	tone_left = 0;
	__enable_irq();
}

int hal_tone_busy(void)
{
	return tone_left != 0;
}

// ---------------------------------------------------------------------------
// Small helpers

void hal_delay(uint32_t ms)
{
	uint32_t t0 = hal_ms;
	while (hal_ms - t0 < ms)
		__WFI();  // light sleep, SysTick wakes us every ms
}

uint32_t hal_entropy(void)
{
	return entropy;
}

void hal_led_level(uint8_t level)
{
	if (level < 1) level = 1;
	if (level > 8) level = 8;
	led_on_ticks = TICKS_PER_MS / 8 * level;
}

// ---------------------------------------------------------------------------
// Standby
//
// In standby the core, the clocks and SysTick stop; only RAM and the EXTI
// logic stay alive. All outputs are parked low so nothing draws current.
// The sensor closing (falling edge on PC2) wakes the chip, and it carries on
// right after __WFI() with the same 24 MHz clock, so nothing needs to be set
// up again except SysTick and the speaker pin.

void hal_sleep_until_shock(void)
{
	hal_tone_off();
	hal_leds = 0;

	SysTick->CTLR = 0;  // stop scanner and clock tick
	SysTick->SR = 0;
	NVIC_DisableIRQ(SysTicK_IRQn);

	// Park every output low: no current through LEDs or the speaker.
	pin_out(GPIOA, 1, 0);
	pin_out(GPIOA, 2, 0);
	pin_out(GPIOC, 1, 0);
	pin_out(GPIOC, 4, 0);
	if (swio_released)
		GPIOD->BCR = 1u << 1;

	// TIM2 stops in standby; the EXTI interrupt wakes us instead.
	uint32_t count = hal_shock_count;
	EXTI->INTFR = EXTI_Line2;
	EXTI->INTENR |= EXTI_Line2;

	PWR->CTLR |= PWR_CTLR_PDDS;  // deep sleep = standby
	PFIC->SCTLR |= (1u << 2);    // SLEEPDEEP
	while (hal_shock_count == count)
		__WFI();
	PFIC->SCTLR &= ~(1u << 2);   // later __WFI() calls are light sleep again
	EXTI->INTENR &= ~EXTI_Line2; // (the ISR already did this if it woke us)
	(void)TIM2->CH1CVR;          // drop captures from before the sleep
	(void)TIM2->CH2CVR;
	TIM2->INTFR = 0;

	speaker_pin_timer();
	systick_start();             // first tick puts the LED lines back to hi-Z
}
