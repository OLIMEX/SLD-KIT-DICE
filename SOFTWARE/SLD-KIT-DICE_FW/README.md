# SLD-KIT-DICE firmware

Firmware for the SLD-KIT-DICE electronic dice: shake it, and it spins and lands on a random number.

- MCU: CH32V003J4M6 (RISC-V, SOP-8), 24 MHz from the internal HSI, no PLL
- Power: one CR2032 coin cell, ~6 µA in standby
- Framework: [ch32fun](https://github.com/cnlohr/ch32fun), built with [PlatformIO](https://platformio.org/)
- Version 1.0.2

Български: [README_BG.md](README_BG.md)

## Hardware

| Pin | Function |
|---|---|
| PA1, PA2, PC1 | 6 charlieplexed LEDs (the dots on the edges) |
| PD1 / SWIO | LED7 (centre dot) **and** the programming pin |
| PC2 | shock sensor (spring switch to GND, normally open), internal pull-up |
| PC4 / TIM1_CH4 | speaker through a 2N3904 transistor |

LED layout (front view):

```
LED1   .    LED6
LED2  LED7  LED5
LED3   .    LED4
```

## Building and programming

You need PlatformIO and a WCH-LinkE programmer. Connect it to the PGM/DBG header: pin 1 = SWIO, pin 2 = GND. **The header has no VCC, so the battery must be in.**

```bash
pio run -t upload
```

That builds and flashes the dice (env `dice`). The other builds:

| env | What it is |
|---|---|
| `dice` | The dice. This is what goes on the boards. |
| `blink` | Hardware test for a newly assembled board: LEDs one by one, a beep, and a flash plus click on every sensor pulse |
| `dice_diag` | The dice with start-up diagnostics (reset cause, crash record) and a 5 s programming window |
| `dice_test` | The dice with SWIO always on and no standby. LED7 stays dark, but the chip can be programmed and its statistics read at any time. Draws ~1.6 mA: for the bench only |

### Ready-to-flash images

The [firmware/](firmware/) folder has the built images, so the dice can be flashed without building anything. After every build, [scripts/copy_firmware.py](scripts/copy_firmware.py) refreshes them from `.pio/build/<env>/firmware.bin`.

| File | Build |
|---|---|
| `firmware/dice.bin` | the dice (this goes on the boards) |
| `firmware/blink.bin` | hardware test |
| `firmware/dice_diag.bin` | dice with start-up diagnostics |
| `firmware/dice_test.bin` | dice with SWIO always on (bench only) |

Each is a raw flash image, written from address `0x08000000`. Use a WCH-LinkE with either tool:

- [minichlink](https://github.com/cnlohr/ch32fun/tree/master/minichlink) (comes with ch32fun and PlatformIO):

  ```bash
  minichlink -w firmware/dice.bin 0x08000000 -b
  ```

- [wlink](https://github.com/ch32-rs/wlink) (`cargo install wlink`):

  ```bash
  wlink --chip CH32V003 flash --address 0x08000000 firmware/dice.bin
  ```

The chip accepts the programmer only in the first 3 s after the battery is put in (see below), so start the command in a loop and then put the battery in:

```bash
until minichlink -w firmware/dice.bin 0x08000000 -b; do sleep 0.2; done
```

To erase the chip:

```bash
until minichlink -E; do sleep 0.2; done
until wlink --chip CH32V003 erase; do sleep 0.2; done
```

### Programming and LED7

LED7 shares its pin with SWIO. So after power-up the program keeps SWIO on for a few seconds (3 s, or 5 s in `dice_diag`), while a dot circles around the edge. Then PD1 becomes the LED7 output, and the chip can't be programmed until the next power-up.

To reprogram: take the battery out, start the upload, and put the battery back. If you miss the window, retry in a loop:

```bash
until pio run -t upload; do sleep 0.2; done
```

## How the dice works

- **Power-up:** the dot circles for 3 s, there is a short beep, and the chip goes to standby.
- **Shake:** the shock sensor wakes the chip. While pulses keep coming, the LEDs flicker and the speaker rattles.
- **Spin:** 300 ms after the last pulse the dice spins: 17 faces, starting at 40 ms each and 15% longer at every step (~2.4 s), with a tick each time. The same face never shows twice in a row.
- **Result:** a short jingle (a fanfare for 6, a sad one for 1). The result stays on for 4 s, blinks, and the chip goes back to standby.
- **Shake again** during the spin or the result, and a new roll starts.

All timings, the brightness (`LED_LEVEL`) and the volume (`SOUND_DUTY`) are at the top of [src/dice/main.c](src/dice/main.c).

### Shock sensor

The sensor is a spring that bounces against a contact. A hard shake gives thousands of very short pulses, some under 10 µs. Instead of an interrupt per pulse, TIM2 counts at 24 MHz and captures the time of each closing (CH2, falling edge) and opening (CH1, rising edge) on PC2. The 1 ms SysTick interrupt reads the captures, so the sensor can't flood the CPU. In standby TIM2 is stopped and an EXTI interrupt on PC2 wakes the chip.

### Randomness

The 24 MHz edge times (when each contact happened, the time between contacts, how long each lasted) are collected in an entropy pool. A spring bouncing at microsecond scale doesn't repeat, even if the hand movement does. Before each roll the pool is hashed (murmur3 finaliser) into the state of a xorshift32 generator. Faces 1 to 6 are picked with rejection sampling, so none is favoured. A simulation of 60 000 rolls gave an even distribution even without sensor entropy.

### Power and battery

- **Standby:** ~6 µA measured. Measure with the programmer disconnected, after the result has gone off.
- **LEDs:** scanned in 4 slots of 1 ms, at most 2 lit at once, dimmed to 3/8 of each slot.
- **Speaker:** PWM at 20% duty. The LEDs and the speaker are never on at the same time.
- **No battery check:** the dice never measures the battery and never turns the sound off by itself. Short dips of the supply under the speaker's load made the earlier checks (v1.0.0 and v1.0.1) turn the sound off while the battery was still good.

The CH32V003 needs 2.7 V and resets at about 2.5 V. A new CR2032 gives ~3.0 V, which drops under the speaker's load as the cell ages.

### Short power dips

On a hard shake the battery can jump in its holder for a moment and reset the chip. RAM keeps its contents through such a short dip. So at every phase change the dice leaves a marker in RAM, and after a power-on reset:

- **Valid marker, dice was busy** (shaking, spinning or showing a result): it carries on with a new roll at once, without the power-up animation. From the outside it looks as if the dice just kept going.
- **No valid marker** (the battery was out long enough for RAM to be lost), or the dice was asleep: a normal power-up with the programming window.

After 6 resets in a row, the dice stops resuming and does a normal power-up.

## Debugging

### Statistics

The dice counts its results in RAM at `0x20000420`. The counters are cleared when the battery is put in, but kept through short power dips. With `dice_test` they can be read at any time:

```bash
~/.platformio/packages/tool-minichlink/minichlink -r stats.bin 0x20000420 56 -e && od -A d -t u4 stats.bin
```

`-e` lets the chip run on after the read. The words, in order:

| Word | Content |
|---|---|
| 0 | magic `0x57A75000` |
| 1–6 | how many times 1 to 6 came up |
| 7 | total rolls |
| 8–9 | not used (0) |
| 10 | how many times the dice resumed after a power dip |
| 11–13 | internal: resume marker, its inverse, random state |

For a fair check you need at least 60 rolls, better 120 or more. With 60 rolls it is normal for one face to come up 5 times and another 15 times.

### Start-up diagnostics (`dice_diag`)

Before the circling dot there are 3 parts of 1.5 s each:

1. **Slow blink:** reset flags. LED1 = power-on, LED2 = NRST pin, LED3 = software reset (a crash was caught), LED4/LED5 = watchdogs, LED6 = low-power reset.
2. **Steady:** only after a crash. The bits of `mcause`: LED1 to LED5 = bits 0 to 4, LED6 = interrupt. LED2 alone = illegal instruction.
3. **Fast blink:** what the dice was doing before the reset. LED1 = power-up, LED2 = sleep, LED3 = shaking, LED4 = spinning, LED5 = result. Dark = unknown, e.g. after the battery was put in.

On a fault (HardFault or NMI) the handler saves a record at `0x20000400` and resets the chip. `dice_diag` then stops after the diagnostics, with LED3 and the `mcause` LEDs blinking slowly and SWIO left on, until the battery is taken out. Read the record with:

```bash
~/.platformio/packages/tool-minichlink/minichlink -r crash.bin 0x20000400 24 && od -A x -t x4 crash.bin
```

The words: magic (`deadd1ce`), mcause, mepc (address of the fault), mtval, INTSYSCR, number of crashes. Turn the address into a line of code with:

```bash
~/.platformio/packages/toolchain-riscv/bin/riscv-wch-elf-addr2line -f -e .pio/build/dice_diag/firmware.elf 0x<mepc>
```

## Files

| File | Content |
|---|---|
| [include/hal.h](include/hal.h), [src/hal.c](src/hal.c) | Hardware layer: LED scanning, speaker, shock sensor, standby |
| [src/dice/main.c](src/dice/main.c) | The dice |
| [src/blink/main.c](src/blink/main.c) | Hardware test |
| [include/funconfig.h](include/funconfig.h) | ch32fun settings (clock, SysTick) |
| [platformio.ini](platformio.ini) | Build environments |
| [scripts/copy_firmware.py](scripts/copy_firmware.py) | Copies each build's firmware.bin to firmware/<env>.bin |
| [firmware/](firmware/) | Ready-to-flash images |

## License

Copyright (c) 2026 OLIMEX LTD BULGARIA. The firmware is released under the [MIT License](LICENSE). It is built on [ch32fun](https://github.com/cnlohr/ch32fun), which is also under the MIT license.
