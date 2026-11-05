// SPDX-License-Identifier: MIT
// Copyright (c) 2026 OLIMEX LTD BULGARIA
//
// SLD-KIT-DICE - hardware test
//
// Flash this first on a newly assembled board:
//
// 1. First PROG_WINDOW_MS after power-up: LED1..LED6 light one by one, in
//    number order, so each LED's position can be checked against the
//    silkscreen. PD1 is left as SWIO, so the chip can be programmed now.
// 2. Then (if USE_LED7 is 1) SWIO is switched off and PD1 drives LED7.
// 3. Loop: LED1..LED7 one by one, a short beep at the start of each round.
//    Every shock sensor pulse: all LEDs flash and the speaker clicks.
//
// Set USE_LED7 to 0 to keep SWIO on all the time (LED7 stays off), e.g. to
// program the board again and again without taking the battery out.

#include "hal.h"

#define USE_LED7        1
#define PROG_WINDOW_MS  5000
#define STEP_MS         300

static const uint8_t order[7] = { LED1, LED2, LED3, LED4, LED5, LED6, LED7 };

int main(void)
{
	SystemInit();
	hal_init();

	// Programming window: PD1 untouched, so only LED1..LED6.
	while (hal_ms < PROG_WINDOW_MS) {
		hal_leds = order[(hal_ms / STEP_MS) % 6];
		hal_delay(1);
	}
	hal_leds = 0;

	if (USE_LED7)
		hal_release_swio();

	hal_tone(SPK_RESONANCE_HZ, 150);

	uint32_t seen = hal_shock_count;
	uint32_t flash_until = 0;
	uint8_t step = 0;
	uint32_t next = hal_ms;

	for (;;) {
		// Sensor pulse: flash everything for 100 ms and click.
		if (hal_shock_count != seen) {
			seen = hal_shock_count;
			flash_until = hal_ms + 100;
			if (!hal_tone_busy())
				hal_tone(SPK_RESONANCE_HZ, 3);
		}

		// Next LED every STEP_MS, a beep at the start of each round.
		if ((int32_t)(hal_ms - next) >= 0) {
			next += STEP_MS;
			step = (step + 1) % 7;
			if (step == 0)
				hal_tone(SPK_RESONANCE_HZ, 40);
		}

		if ((int32_t)(flash_until - hal_ms) > 0)
			hal_leds = LED_ALL;
		else
			hal_leds = order[step];

		hal_delay(1);
	}
}
