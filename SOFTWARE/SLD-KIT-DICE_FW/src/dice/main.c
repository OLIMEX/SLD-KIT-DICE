// SPDX-License-Identifier: MIT
// Copyright (c) 2026 OLIMEX LTD BULGARIA
//
// SLD-KIT-DICE - electronic dice
//
// What it does
//   Power-up  The LEDs circle around the edge for PROG_WINDOW_MS. Meanwhile
//             PD1 is still the programming pin (SWIO), so the chip can be
//             programmed. Then PD1 becomes LED7, a short beep, and standby.
//   Shake     The shock sensor wakes the chip. While pulses keep coming, the
//             LEDs flicker and the speaker rattles.
//   Spin      SETTLE_MS after the last pulse the dice "spins": SPIN_STEPS
//             faces, each shown a bit longer than the one before, with a tick.
//   Result    The last face is the result: a short jingle (a fanfare for 6,
//             a sad one for 1), RESULT_MS on, a blink, and back to standby
//             (~6 uA). Shaking again during the spin or the result starts a
//             new roll.
//   Power dip A hard shake can make the battery jump in its holder and reset
//             the chip. RAM survives such a short dip, so if the dice was busy
//             it carries on with a new roll at once (see warm_restart()).
//
// State flow (main):
//
//   power_up -> sleep -> shake -> spin -> result -> sleep
//                          ^        |        |
//                          +--------+--------+  shaken again

#include "hal.h"

#define FW_VERSION "1.0.2"

// ---- build options (set from platformio.ini) --------------------------------
#ifndef DIAG
#define DIAG             0     // 1 = start-up diagnostics, crash hold, 5 s window
#endif                         //     (env dice_diag); 0 = release build
#ifndef KEEP_SWIO
#define KEEP_SWIO        0     // 1 = never turn SWIO off (env dice_test): LED7 stays
#endif                         //     dark, but RAM can be read at any time

// ---- tuning ----------------------------------------------------------------
#define PROG_WINDOW_MS   (DIAG ? 5000 : 3000)  // after power-up, before PD1 becomes LED7
#define LED_LEVEL        3     // LED on-time per scan slot, 1..8 (8 = brightest)
#define SOUND_DUTY       20    // speaker PWM duty %, 50 = loudest
#define SOUND_ON         1     // 0 = silent dice
#define SETTLE_MS        300   // quiet time after shaking before the spin
#define SHAKE_MAX_MS     20000 // roll anyway if pulses never stop
#define SHAKE_FRAME_MS   60    // flicker speed while shaking
#define SHAKE_CLICK_GAP  40    // min ms between rattle clicks
#define SPIN_STEPS       17    // faces shown, including the result
#define SPIN_FIRST_MS    40    // first spin step
#define SPIN_GROWTH_PCT  115   // each step is this % of the previous one (~2.4 s)
#define RESULT_MS        4000  // how long the result stays on
#define RESHAKE_EVENTS   1     // ms ticks with sensor closings that restart a
                               // spin or a shown result (1 = most sensitive)
#define LAND_MS          400   // result shown silently before the jingle
#define NOTE_GAP_MS      70    // LEDs light between jingle notes
#define TICK_HZ          SPK_RESONANCE_HZ
#define TICK_MS          3     // spin tick length
#define WARM_STREAK_MAX  6     // warm restarts in a row (any): give up resuming
#define CRASH_HOLD       DIAG  // after a caught crash: stop, keep SWIO on (see power_up)

// What the dice is doing. Reported to hal_set_phase(), so that the diagnostics
// can show what was going on before a reset, and kept in the resume marker.
enum { PH_POWERUP = 1, PH_SLEEP, PH_SHAKE, PH_SPIN, PH_RESULT };

// ---- statistics ------------------------------------------------------------
// Kept in RAM that survives resets and short power dips (hal.h). Cleared when
// the battery is inserted. With env dice_test they can be read at any time:
//   minichlink -r stats.bin 0x20000420 56 -e && od -A d -t u4 stats.bin
// words: [0] magic, [1..6] how many times 1..6 came up, [7] total rolls,
//        [8], [9] not used (0),
//        [10] warm restarts (resumed after a power dip),
//        [11] resume marker, [12] ~marker (see keep_save), [13] random state.
#define STATS_MAGIC 0x57A75000u
#define STATS       HAL_APP_RAM
#define STATS_WORDS 14
#define KEEP_MAGIC  0x4B000000u

static void stats_init(void)
{
	for (int i = 1; i < STATS_WORDS; i++) STATS[i] = 0;
	STATS[0] = STATS_MAGIC;
}

// ---- random numbers ----------------------------------------------------------
// The result must not depend on how the dice is shaken in a way a player
// could repeat. The randomness comes from the sensor: hal.c collects the
// 24 MHz capture times of its edges (when each contact happened, the time
// between contacts, how long each lasted). A spring bouncing at microsecond
// scale is not repeatable, even if the hand movement is.
//
// Before each roll the pool is hashed together with the pulse count and the
// time, and the result is mixed into the state of a xorshift32 generator,
// which then gives the faces. A simulation of 60 000 rolls gave an even
// distribution even with no sensor entropy at all.
static uint32_t rng = 0x2545F491;

static uint32_t hash32(uint32_t x)  // murmur3 finaliser: every input bit
{                                   // affects every output bit
	x ^= x >> 16; x *= 0x85EBCA6Bu;
	x ^= x >> 13; x *= 0xC2B2AE35u;
	x ^= x >> 16;
	return x;
}

static void rng_mix(void)
{
	rng = hash32(rng ^ hash32(hal_entropy() ^ (hal_shock_count << 20) ^ hal_ms));
	if (!rng) rng = 0x2545F491;  // xorshift must never be 0
	STATS[13] = rng;             // a warm restart continues from here
}

static uint32_t rnd(void)        // xorshift32 (Marsaglia)
{
	rng ^= rng << 13;
	rng ^= rng >> 17;
	rng ^= rng << 5;
	return rng;
}

// Uniform 1..6: values above the largest multiple of 6 are thrown away,
// so no face is even slightly favoured.
static uint8_t rnd_face(void)
{
	uint32_t r;
	do r = rnd(); while (r >= 0xFFFFFFFCu);  // 0xFFFFFFFC = 6 * 715827882
	return 1 + r % 6;
}

// Random face different from a and b (0 = no restriction).
static uint8_t rnd_face_except(uint8_t a, uint8_t b)
{
	uint8_t f;
	do f = rnd_face(); while (f == a || f == b);
	return f;
}

// ---- display -----------------------------------------------------------------
// LEDs for each face. 2 and 3 use one of the two diagonals at random, like a
// real die seen from different sides.
static uint8_t face_mask(uint8_t n)
{
	uint8_t diag = (rnd() & 1) ? (LED1 | LED4) : (LED6 | LED3);
	switch (n) {
	case 1:  return LED7;
	case 2:  return diag;
	case 3:  return diag | LED7;
	case 4:  return LED1 | LED6 | LED3 | LED4;
	case 5:  return LED1 | LED6 | LED3 | LED4 | LED7;
	case 6:  return LED1 | LED2 | LED3 | LED4 | LED5 | LED6;
	default: return 0;
	}
}

// ---- surviving short power dips --------------------------------------------
// Every phase change leaves a marker in RAM: the phase and how many warm
// restarts came in a row. The streak is cleared when a jingle plays to the end.
//
// After a power-on reset, a valid marker means RAM kept its contents, i.e.
// the supply only dipped (the battery jumped in its holder). A battery taken
// out for longer leaves random RAM, and the dice starts normally.
static uint8_t cur_phase, warm_streak;

// marker: [31:24] KEEP_MAGIC, [23:16] warm_streak, [7:0] phase.
// It is stored twice, the second time inverted, so random RAM can't pass
// for a valid marker.
static void keep_save(void)
{
	uint32_t k = KEEP_MAGIC | (uint32_t)warm_streak << 16 | cur_phase;
	STATS[11] = k;
	STATS[12] = ~k;
}

static void set_phase(uint8_t p)
{
	cur_phase = p;
	hal_set_phase(p);
	keep_save();
}

// ---- sound -------------------------------------------------------------------

static void tone(uint16_t hz, uint16_t ms)
{
	if (SOUND_ON) hal_tone(hz, ms);
}

// Wait ms; return 1 early if the dice was shaken again since `base`
// (a value of hal_shock_events taken earlier).
static int wait_or_reshake(uint32_t ms, uint32_t base)
{
	uint32_t t0 = hal_ms;
	while (hal_ms - t0 < ms) {
		if (hal_shock_events - base >= RESHAKE_EVENTS)
			return 1;
		__WFI();
	}
	return 0;
}

typedef struct { uint16_t hz, ms; } note_t;  // a list ends with ms = 0

static const note_t jingle_normal[] = { {2400, 60}, {2700, 140}, {0, 0} };
static const note_t jingle_six[]    = { {2000, 80}, {2400, 80}, {2700, 80}, {3200, 300}, {0, 0} };
static const note_t jingle_one[]    = { {2700, 120}, {2000, 300}, {0, 0} };
static const note_t jingle_hello[]  = { {2700, 60}, {0, 0} };

// Play notes; return 0 if shaken again.
// The LEDs are dark while a note sounds (see hal.c), so whatever is in
// hal_leds blinks in the gaps between notes, in time with the jingle.
static int play(const note_t *n, uint32_t base)
{
	for (; n->ms; n++) {
		tone(n->hz, n->ms);
		if (wait_or_reshake(n->ms + NOTE_GAP_MS, base))
			return 0;
	}
	return 1;
}

// ---- phases ------------------------------------------------------------------

// Flicker while shaking; returns once pulses stop for SETTLE_MS.
static void shake_phase(void)
{
	set_phase(PH_SHAKE);
	uint32_t start = hal_ms;
	uint32_t seen = hal_shock_count;
	uint32_t next_frame = hal_ms;
	uint32_t last_click = hal_ms - SHAKE_CLICK_GAP;

	for (;;) {
		uint32_t last = hal_shock_ms;
		uint32_t now = hal_ms;
		if (now - last >= SETTLE_MS || now - start >= SHAKE_MAX_MS)
			break;

		if (hal_shock_count != seen) {          // new pulse: rattle
			seen = hal_shock_count;
			if (now - last_click >= SHAKE_CLICK_GAP) {
				tone(TICK_HZ, 2);
				last_click = now;
			}
		}
		if ((int32_t)(now - next_frame) >= 0) { // next random face
			next_frame = now + SHAKE_FRAME_MS;
			hal_leds = face_mask(rnd_face());
		}
		__WFI();
	}
}

// Spin with a growing delay; the face after the last one shown is `result`.
// Returns 0 if shaken again.
static int spin_phase(uint8_t result)
{
	set_phase(PH_SPIN);
	uint32_t base = hal_shock_events;
	uint32_t d = SPIN_FIRST_MS;
	uint8_t prev = 0;

	for (int i = 0; i < SPIN_STEPS - 1; i++) {
		// never the same face twice in a row, including before the result
		uint8_t f = rnd_face_except(prev, i == SPIN_STEPS - 2 ? result : 0);
		hal_leds = face_mask(f);
		tone(TICK_HZ, TICK_MS);
		if (wait_or_reshake(d, base))
			return 0;
		d = d * SPIN_GROWTH_PCT / 100;
		prev = f;
	}
	return 1;
}

// Show the result with a jingle, then blink off. Returns 0 if shaken again.
static int result_phase(uint8_t result)
{
	set_phase(PH_RESULT);

	if (STATS[0] != STATS_MAGIC)
		stats_init();
	STATS[result]++;
	STATS[7]++;

	uint32_t base = hal_shock_events;
	uint8_t mask = face_mask(result);
	hal_leds = mask;
	if (wait_or_reshake(LAND_MS, base))
		return 0;

	const note_t *j = result == 6 ? jingle_six : result == 1 ? jingle_one : jingle_normal;
	if (!play(j, base))
		return 0;
	warm_streak = 0;    // the jingle finished without a reset
	if (wait_or_reshake(RESULT_MS, base))
		return 0;

	for (int i = 0; i < 2; i++) {
		hal_leds = 0;
		if (wait_or_reshake(150, base)) return 0;
		hal_leds = mask;
		if (wait_or_reshake(150, base)) return 0;
	}
	hal_leds = 0;
	return 1;
}

// ---- start-up diagnostics (DIAG builds only) ---------------------------------
// Shown for RESET_CAUSE_MS before the ring, in three parts:
//
// 1. SLOW BLINK = reset flags:
//      LED1 power-on / brown-out   LED4 independent watchdog
//      LED2 NRST pin               LED5 window watchdog
//      LED3 software reset         LED6 low-power (standby) reset
//    Battery insertion normally shows LED1 (+ LED2). LED3 = a crash was caught.
//
// 2. STEADY = trap cause (mcause), only after a crash:
//      LED1..LED5 = mcause bits 0..4, LED6 = interrupt (bit 31).
//      LED2 alone (2) = illegal instruction. Dark = no crash.
//
// 3. FAST BLINK = what the dice was doing before the reset:
//      LED1 power-up, LED2 sleep / waking up, LED3 shaking, LED4 spinning,
//      LED5 showing the result. Dark = unknown (e.g. after power-on).
#define DIAG_PART_MS   1500
#define RESET_CAUSE_MS (3 * DIAG_PART_MS)

static uint8_t mcause_leds(uint32_t c)
{
	return (c & 0x1F) | ((c >> 31) ? LED6 : 0);
}

static void show_reset_cause(void)
{
	uint32_t f = hal_reset_flags;
	uint8_t flags = 0;
	if (f & RCC_PORRSTF)  flags |= LED1;
	if (f & RCC_PINRSTF)  flags |= LED2;
	if (f & RCC_SFTRSTF)  flags |= LED3;
	if (f & RCC_IWDGRSTF) flags |= LED4;
	if (f & RCC_WWDGRSTF) flags |= LED5;
	if (f & RCC_LPWRRSTF) flags |= LED6;

	uint8_t trap = 0;
	if (hal_last_crash.magic == HAL_CRASH_MAGIC && (f & RCC_SFTRSTF))
		trap = mcause_leds(hal_last_crash.mcause);   // caught by HardFault/NMI
	else if (!flags)
		trap = mcause_leds(hal_boot_mcause);          // jumped to address 0

	static const uint8_t phase_led[6] = { 0, LED1, LED2, LED3, LED4, LED5 };
	uint8_t phase = hal_last_phase < 6 ? phase_led[hal_last_phase] : 0;

	while (hal_ms < DIAG_PART_MS) {
		hal_leds = (hal_ms % 500 < 400) ? flags : 0;
		hal_delay(1);
	}
	hal_leds = trap;
	while (hal_ms < 2 * DIAG_PART_MS)
		hal_delay(1);
	while (hal_ms < 3 * DIAG_PART_MS) {
		hal_leds = (hal_ms % 200 < 100) ? phase : 0;
		hal_delay(1);
	}
}

// ---- power-up ------------------------------------------------------------------

// Power-on reset with RAM intact and the dice busy before it: the supply only
// dipped during a shake. Returns 1 if the dice should carry on rolling.
static int warm_restart(void)
{
	if (!(hal_reset_flags & RCC_PORRSTF))
		return 0;                     // crash or pin reset: normal start
	uint32_t k = STATS[11];
	if (STATS[0] != STATS_MAGIC || STATS[12] != ~k ||
	    (k & 0xFF000000u) != KEEP_MAGIC)
		return 0;                     // RAM lost: battery inserted
	uint8_t p = k & 0xFF;
	if (p != PH_SHAKE && p != PH_SPIN && p != PH_RESULT)
		return 0;                     // was asleep: e.g. battery swapped
	uint8_t streak = (k >> 16) & 0xFF;
	if (streak >= WARM_STREAK_MAX)
		return 0;                     // keeps resetting: start normally

	warm_streak = streak + 1;
	if (STATS[13])
		rng = STATS[13];
	STATS[10]++;
	return 1;
}

// Returns 1 after a warm restart: go straight to a new roll.
static int power_up(void)
{
	if (warm_restart())
		return 1;  // SWIO stays on until the spin: still programmable meanwhile

	set_phase(PH_POWERUP);
	if ((hal_reset_flags & RCC_PORRSTF) || STATS[0] != STATS_MAGIC)
		stats_init();  // RAM is random after power-on
	keep_save();

	if (DIAG)
		show_reset_cause();

	// After a caught crash, stay here with SWIO on so the crash record can be
	// read at leisure (minichlink, see README). LED3 + the mcause LEDs blink
	// slowly. Take the battery out to continue.
	if (CRASH_HOLD && hal_last_crash.magic == HAL_CRASH_MAGIC &&
	    (hal_reset_flags & RCC_SFTRSTF)) {
		uint8_t m = LED3 | mcause_leds(hal_last_crash.mcause);
		for (;;) {
			hal_leds = (hal_ms % 2000 < 1000) ? m : 0;
			hal_delay(1);
		}
	}

	// Programming window: a dot circles around the edge. PD1/LED7 is not
	// touched, so a programmer can connect now.
	static const uint8_t ring[6] = { LED1, LED6, LED5, LED4, LED3, LED2 };
	uint32_t window_end = (DIAG ? RESET_CAUSE_MS : 0) + PROG_WINDOW_MS;
	while (hal_ms < window_end) {
		hal_leds = ring[(hal_ms / 100) % 6];
		hal_delay(1);
	}
	hal_leds = 0;
	if (!KEEP_SWIO)
		hal_release_swio();

	hal_leds = LED_ALL;
	play(jingle_hello, hal_shock_events);
	hal_leds = 0;
	return 0;
}

// ---- main ------------------------------------------------------------------------

int main(void)
{
	SystemInit();
	hal_init();
	hal_led_level(LED_LEVEL);
	hal_tone_duty = SOUND_DUTY;
	int warm = power_up();

	for (;;) {
		if (warm) {
			warm = 0;       // resume: the shake phase picks up where it was
		} else {
			warm_streak = 0;
			set_phase(PH_SLEEP);
			if (KEEP_SWIO) {
				// No standby: a programmer can't reach a chip in standby.
				uint32_t e = hal_shock_events;
				while (hal_shock_events == e)
					hal_delay(1);
			} else {
				hal_sleep_until_shock();
			}
		}

		// One roll. Shaking again during the spin or the result goes back
		// to the shake phase.
		for (;;) {
			shake_phase();
			rng_mix();
			uint8_t result = rnd_face();
			if (!KEEP_SWIO && !hal_swio_released())
				hal_release_swio();  // only after a warm restart
			if (!spin_phase(result))
				continue;
			if (!result_phase(result))
				continue;
			break;
		}
	}
}
