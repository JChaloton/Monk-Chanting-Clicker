# Firmware plan

Target: CH32V003F4P6. Toolchain: **ch32v003fun** (open-source, has examples for
standby-wake, PWM, and SPI) or MounRiver Studio. Programmer: WCH-LinkE over SWIO (PD1).

## The whole loop

```
standby (~9 µA, flash in deep power-down, amp SD low)
  └─ EXTI on PD3 falling edge (press)
       ├─ debounce ~20 ms, re-read PD3; if high again, go back to sleep
       ├─ wake flash        : CS# low, 0xAB, CS# high, wait tRES1 (~3 µs, check datasheet)
       ├─ enable amp        : PC3 = HIGH, wait a few ms for it to settle
       ├─ prime buffer A + B from flash
       ├─ start TIM1 CH1 PWM (187.5 kHz carrier) + 8 kHz sample tick
       │    ISR: write next sample to CH1 compare, advance; on buffer end, swap and
       │         flag the main loop to refill the now-empty one
       │    main: refill the flagged buffer over SPI (256 B page)
       ├─ end of chant: ramp duty gently to 50 %, stop the timer   ← anti-pop
       ├─ PC3 = LOW (amp off)
       ├─ flash deep power-down: CS# low, 0xB9, CS# high
       └─ back to standby
```

Presses during playback: **decide and document** — either ignore them, or restart the
chant. See `docs/decisions.md` (open item).

## Audio path details

- **Carrier 187.5 kHz** on TIM1 CH1. At 48 MHz core: 48 MHz / 256 = 187500 Hz, i.e. an
  8-bit PWM with period 256 and no prescaler. That is exactly why the sample depth is
  8 bits — one sample maps straight to the compare register.
- **Sample rate 8 kHz** → one new duty value every **125 µs**. Drive this from a second
  timer interrupt, or from a TIM1 update-event counter that fires every 24th PWM period
  (187500 / 8000 = 23.4375 — so prefer a dedicated tick, or run the PWM at 192 kHz-ish
  and accept the small carrier shift; verify the divisor you pick).
- **Anti-pop:** the PWM idle level is 50 % duty (≈1.5 V), not 0. Ramp from 50 % into the
  first sample and back to 50 % after the last, over a few ms, before dropping SD.

## Double buffer

Two 256-byte buffers in RAM — one flash page each — out of 2 KB total.

- ISR plays out of the active buffer only. It never touches SPI.
- Main loop refills the inactive buffer. A 256-byte page at 8 kHz is 32 ms of audio;
  the SPI refill takes well under a millisecond of bus time. Enormous slack — the
  discipline of the swap, not the bandwidth, is what prevents stutter.
- Use a `volatile` flag/index pair between ISR and main loop, and keep the ISR short.

RAM budget: 2 × 256 B buffers = 512 B of 2048 B. Leave room for stack and the ADPCM
decoder state if compression is used.

## Storage format

Two options, decided at build time:

| Format | Capacity in 1 MB | Decoder cost |
|---|---|---|
| Raw 8-bit unsigned PCM, 8 kHz | ~2 min | none — byte goes straight to the compare register |
| 4-bit IMA ADPCM, 8 kHz | 4+ min | small table-driven step decode, well within this chip |

Start with **raw 8-bit** for first sound. Move to ADPCM only if the chant does not fit.
Flash layout is described in `docs/audio-pipeline.md`.

## Low power

The sleep budget is the product. Order matters on the way down:

1. stop the timer / disable the PWM output
2. PC3 LOW (amp → < 1 µA)
3. flash `B9h` deep power-down (→ < 1 µA)
4. MCU standby with EXTI on PD3 armed (→ ~9 µA total)

Skipping step 3 alone leaves tens of µA of flash idle current and wrecks the estimate.
Any new feature must state its idle current.

## SPI flash commands used

| Cmd | Meaning | Notes |
|---|---|---|
| `9Fh` | JEDEC ID | must return `A1 40 14` — the one-shot proof all four SPI wires work |
| `03h` | read data (low speed) | address is 3 bytes, big-endian; fine for 64 kbit/s streaming |
| `0Bh` | fast read | needs one dummy byte; use if pushing clock rate |
| `06h` | write enable | before any program/erase |
| `02h` | page program | 256 B page, must not cross a page boundary |
| `20h` / `D8h` | sector (4 KB) / block (64 KB) erase | erase before program |
| `05h` | read status register 1 | poll BUSY bit after program/erase |
| `B9h` | deep power-down | last thing before MCU standby |
| `ABh` | release from deep power-down | first thing on wake; observe the recovery delay |

## Suggested source layout (stubs are in `src/`)

```
src/board.h       pin/port definitions and timing constants — SINGLE SOURCE OF PIN TRUTH
src/main.c        init, EXTI wake, top-level play sequence
src/spiflash.c/.h SPI init + flash commands (jedec id, read, program, power-down)
src/audio.c/.h    PWM/timer setup, double buffer, sample ISR, ramp helpers
src/loader/       one-off UART→flash loader firmware (see audio-pipeline.md)
```

Never hard-code a port/pin outside `board.h`.

## Build order that mirrors bring-up

1. Blink/toggle PD2 once per second — proves power, solder, programmer, pin numbering.
2. Read SW1 on PD3 with the internal pull-up; toggle PD2 on each press; observe bounce.
3. Fixed 50 % PWM on PD2 — multimeter should read ≈1.5 V after R1, ≈0.16 V at the
   divider node.
4. SPI `9Fh` → expect `A1 40 14`; then write a page and read it back.
5. PC3 HIGH + a 440 Hz PWM tone → clean beep. (With PC3 LOW: silence.)
6. Stream the real chant from flash with the double buffer.
7. Add standby/wake and the power-down sequencing; measure idle current.

`docs/bring-up.md` is the matching hardware checklist.
