// SPDX-License-Identifier: MIT
// Copyright (c) 2026 OLIMEX LTD BULGARIA
//
// SLD-KIT-DICE Rev.A - hardware layer (HAL)
//
// Everything that touches the chip's registers lives in hal.c. The programs
// (src/dice, src/blink) only use the functions and variables declared here.
//
// Pins (CH32V003J4M6, SOP-8). Some SOP-8 pins are bonded to two pads inside
// the package; the second pad is kept as a floating input so it never fights
// the first one.
//   pin 1  PA1 (+PD6 bonded)    charlieplex line
//   pin 2  VSS
//   pin 3  PA2                  charlieplex line
//   pin 4  VDD                  CR2032, 2.7..3.0 V
//   pin 5  PC1                  charlieplex line
//   pin 6  PC2                  shock sensor to GND, internal pull-up
//   pin 7  PC4 / TIM1_CH4       speaker via 2N3904 (high = coil on)
//   pin 8  PD1/SWIO (+PD4,PD5)  LED7 (centre) to GND via 100R, and the
//                               programming pin (see hal_release_swio)
//
// LED layout (front view):     Charlieplex (anode -> cathode):
//   LED1   .    LED6            LED1 PA1->PA2   LED2 PA2->PA1
//   LED2  LED7  LED5            LED3 PA1->PC1   LED4 PC1->PA1
//   LED3   .    LED4            LED5 PA2->PC1   LED6 PC1->PA2
//
// Charlieplexing: 3 pins drive 6 LEDs. To light LED1, PA1 goes high, PA2 low
// and PC1 is left floating (input), so current can flow only through LED1.
// Only LEDs that share the same high pin can be on at once, so the LEDs are
// shown one group at a time, fast enough (250 Hz) that the eye sees them all.

#ifndef HAL_H
#define HAL_H

#include <stdint.h>
#include "ch32fun.h"

// ---- LEDs ------------------------------------------------------------------
// hal_leds is a bit mask of these. Write it at any time; the scanner in the
// SysTick interrupt shows it from the next millisecond on.
#define LED1 (1u << 0)  // top left
#define LED2 (1u << 1)  // middle left
#define LED3 (1u << 2)  // bottom left
#define LED4 (1u << 3)  // bottom right
#define LED5 (1u << 4)  // middle right
#define LED6 (1u << 5)  // top right
#define LED7 (1u << 6)  // centre (PD1/SWIO, lit only after hal_release_swio)
#define LED_ALL 0x7Fu

#define SPK_RESONANCE_HZ 2700   // QMB-09B-03 speaker is loudest here

// ---- state shared with the interrupts --------------------------------------
extern volatile uint32_t hal_ms;          // milliseconds since start (stops in standby)
extern volatile uint8_t  hal_leds;        // LED bit mask shown by the scanner
extern volatile uint32_t hal_shock_count; // sensor closings (approximate: max 2 per ms)
extern volatile uint32_t hal_shock_ms;    // hal_ms of the last closing
extern volatile uint32_t hal_shock_events;// ms ticks that saw at least one closing
extern uint32_t hal_reset_flags;          // RCC->RSTSCKR at start (RCC_*RSTF bits)
extern uint32_t hal_boot_mcause;          // mcause at start; with no reset flags set,
                                          // non-zero = we got here through a trap

// ---- RAM kept across resets ------------------------------------------------
// The top of RAM is not touched by the start-up code (it is outside .data and
// .bss), so it keeps its contents through a reset, and also through a power
// dip short enough for the RAM cells to hold their charge. After the battery
// has been out for a while it holds random values, so everything stored here
// carries a "magic" word that marks it as valid.
//   0x20000400  crash record (24 bytes, hal_crash_t)
//   0x20000418  phase the application last reported (hal_set_phase)
//   0x2000041C  not used
//   0x20000420  free for the application (dice: statistics and resume marker)
// The stack grows down from 0x20000800. The dice uses a few hundred bytes of
// it at most, well clear of the ~0x3A0 bytes above the dice's data.

// Crash record. HardFault/NMI store it here and reset the chip, instead of
// hanging. Read it with:  minichlink -r crash.bin 0x20000400 24
typedef struct {
	uint32_t magic;     // HAL_CRASH_MAGIC when valid
	uint32_t mcause;    // 2 = illegal instruction, 1/5/7 = access faults, ...
	uint32_t mepc;      // address of the faulting instruction (see addr2line)
	uint32_t mtval;     // bad address / bad instruction
	uint32_t intsyscr;  // CSR 0x804: bit0 hardware stack, bit1 nesting
	uint32_t count;     // crashes since power-on
} hal_crash_t;

#define HAL_CRASH_ADDR  0x20000400u
#define HAL_CRASH       ((volatile hal_crash_t *)HAL_CRASH_ADDR)
#define HAL_CRASH_MAGIC 0xDEADD1CEu

extern hal_crash_t hal_last_crash;        // copy taken at start, magic = 0 if none

#define HAL_PHASE_ADDR  0x20000418u
#define HAL_PHASE       (*(volatile uint32_t *)HAL_PHASE_ADDR)
#define HAL_PHASE_MAGIC 0x50480000u
#define HAL_APP_RAM     ((volatile uint32_t *)0x20000420u)

// The application reports what it is doing, so a reset can be traced back.
static inline void hal_set_phase(uint8_t p) { HAL_PHASE = HAL_PHASE_MAGIC | p; }
extern uint32_t hal_last_phase;           // phase before the last reset, 0 = unknown

// ---- functions -------------------------------------------------------------

// Clocks, pins, timers, interrupts. Call once, right after SystemInit().
void hal_init(void);

// Turn PD1 from SWIO into a GPIO for LED7. After this the chip can be
// programmed only right after power-up (take the battery out and back in).
void hal_release_swio(void);
int  hal_swio_released(void);

// Speaker. Non-blocking: the tone stops by itself after ms (ms = 0: until
// hal_tone_off). The LEDs are dark while a tone plays, so the battery never
// has to feed both at once.
void hal_tone(uint16_t hz, uint16_t ms);
void hal_tone_off(void);
int  hal_tone_busy(void);
extern volatile uint8_t hal_tone_duty;    // PWM duty %, default 50 (lower = quieter,
                                          // less current)

// LED on-time in each 1 ms scan slot, in eighths: 8 = full, 4 = half current.
void hal_led_level(uint8_t level);

// Wait ms milliseconds in light sleep (the CPU halts until the next tick).
void hal_delay(uint32_t ms);

// Standby until the shock sensor fires (~6 uA). LEDs and sound go off, and
// hal_ms stops while asleep.
void hal_sleep_until_shock(void);

// Entropy pool fed by the sensor's edge times (24 MHz captures). Hash it
// before use; see rng_mix() in src/dice/main.c.
uint32_t hal_entropy(void);

#endif
