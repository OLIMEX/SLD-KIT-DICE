# SLD-KIT-DICE ready-to-flash images

| File | Build |
|---|---|
| `dice.bin` | the dice (this goes on the boards) |
| `blink.bin` | hardware test of a new board |
| `dice_diag.bin` | dice with start-up diagnostics |
| `dice_test.bin` | dice with SWIO always on (bench only) |

Raw flash images for the CH32V003, written from address `0x08000000` with a WCH-LinkE. Connect it to the PGM/DBG header: pin 1 = SWIO, pin 2 = GND. The battery must be in.

The chip accepts the programmer only in the first 3 s after the battery is put in. Start the command, then put the battery in:

```bash
until minichlink -w dice.bin 0x08000000 -b; do sleep 0.2; done
```

or with [wlink](https://github.com/ch32-rs/wlink):

```bash
until wlink --chip CH32V003 flash --address 0x08000000 dice.bin; do sleep 0.2; done
```

Erase the chip:

```bash
until minichlink -E; do sleep 0.2; done
```

These files are refreshed by every PlatformIO build (see [../README.md](../README.md)).
