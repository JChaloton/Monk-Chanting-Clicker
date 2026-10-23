# Firmware design

What the firmware does, the arithmetic behind every constant, and which parts are built.
This was originally a plan; it is now a description of working code, with the one
unbuilt piece (MCU standby) flagged as such.

Target: CH32V003F4P6. Toolchain: **ch32v003fun** (upstream header is `ch32fun.h`,
makefile fragment `ch32fun/ch32fun.mk`). Programmer: WCH-LinkE over SWIO (PD1).

For a file-by-file walkthrough see [`code-tour.md`](code-tour.md).

## The whole loop

```
idle  (today: MCU polling PD3 at a few mA; amp SD low, flash in deep power-down)
  └─ press on PD3 (active low)
       ├─ debounce 20 ms, re-read PD3; if high again, ignore
       ├─ wake flash        : CS# low, ABh, CS# high, wait ~20 us
       ├─ enable amp        : PC3 HIGH, wait 5 ms for it to settle
       ├─ prime buffers A and B from flash (256 B each)
       ├─ ramp PWM duty from mid-scale to the first sample      <- anti-pop
       ├─ start TIM2 at the header's sample rate
       │    ISR : write next byte to TIM1->CH1CVR, advance; at end of a buffer,
       │          swap and flag the main loop to refill the spent one
       │    main: audio_service() refills the flagged buffer over SPI, and
       │          watches a SysTick deadline so a dead ISR cannot spin forever
       ├─ end of chant: stop TIM2, ramp duty back to mid-scale  <- anti-pop
       ├─ 2 ms, so the ramp reaches the amp before it is muted
       ├─ PC3 LOW (amp off, < 1 uA)
       ├─ flash deep power-down: CS# low, B9h, CS# high (< 1 uA)
       ├─ report what happened over printf (never during playback)
       └─ wait for release + 20 ms, back to idle
```

**Presses during playback are ignored.** `play_chant()` blocks, and the loop then waits
for release. Decided, not accidental: a chant that restarts halfway through is worse than
one that finishes. Changing it means polling PD3 inside the service loop.

## Audio path arithmetic

Everything descends from **48 MHz** (internal HSI × 2 PLL, set in `src/funconfig.h`).

| Quantity | Formula | Value |
|---|---|---|
| PWM carrier | 48 MHz / `AUDIO_PWM_PERIOD` (256) | **187.5 kHz**, `TIM1->ATRLR = 255` |
| Sample depth | one byte → one compare value | **8 bits**, 0–255, idle 128 |
| Sample tick | 48 MHz / rate | rate 8000 → 6000, `TIM2->ATRLR = 5999` |
| Sample period | 1 / 8000 | 125 µs |
| Buffer span | 256 B / 8000 | 32 ms |
| SPI bit clock | 48 MHz / (2 << `FLASH_SPI_BR`) | 3 MHz at BR = 3 |
| SPI load while streaming | 8 kB/s at 3 Mbit/s | < 3 % of the bus |

### Why the sample tick is TIM2

187500 / 8000 = **23.4375** — no whole-number division of the TIM1 update event lands on
8 kHz, so the tick cannot come from the carrier without either shifting the carrier or
accepting a pitch error.

Dividing the core clock lands exactly: 48 000 000 / 8000 = 6000.

SysTick divides it just as exactly and was the first choice — and it was wrong, because
**ch32fun builds `Delay_Us()`/`Delay_Ms()` on the free-running SysTick counter**. Taking
SysTick over broke every delay in the firmware and froze the counter on stop, hanging the
board inside `audio_stop()`'s ramp with the amp still enabled. TIM2 is otherwise unused
and leaves the timebase alone. Full account: [`debug-log.md`](debug-log.md).

Keeping SysTick free has a second payoff: `play_range()` measures the *achieved* sample
rate against it, so a TIM2 misconfiguration cannot hide by skewing its own measurement.

### Anti-pop

The PWM idle level is 50 % duty (≈1.5 V), not 0 — the amp input is capacitively coupled
and only responds to change. Stepping straight from 128 to the first sample, or back at
the end, is a step of up to half the rail, which the amp faithfully reproduces as a
click. `ramp_to()` walks it in 64 linear steps over `AUDIO_RAMP_MS` (5 ms) at both ends.

## Double buffer

Two 256-byte buffers — one flash page each — out of 2 KB of RAM.

- The ISR plays out of the active buffer only, and **never touches SPI**.
- The main loop refills the buffer the ISR has finished with. 32 ms of audio per buffer
  against a sub-millisecond refill is enormous slack: the discipline of the swap, not the
  bandwidth, is what prevents stutter.
- Exactly one buffer is ever pending refill, because the ISR is always at least a whole
  buffer ahead. That is why `audio_service()` needs no locking.
- A **short** buffer (`fill < 256`) means end of chant. A **full** buffer followed by an
  empty one means either an exact page-boundary ending or an underrun — both stop, rather
  than replay stale bytes, because looping old audio is far harder to diagnose by ear
  than silence.

RAM budget: 512 B of 2048 B for buffers, plus each loader's separate 256 B page staging
(and, in `loader_swio.c` only, a 544 B host mailbox at a fixed `0x20000300`)
in its own firmware. Leave room for stack and, if it is ever written, ADPCM state.

## Storage format

| Format | Capacity in 1 MB | Decoder cost | Status |
|---|---|---|---|
| Raw 8-bit unsigned PCM, 8 kHz | ~2 min | none — byte goes straight to the compare register | **implemented, shipping** |
| 4-bit IMA ADPCM, 8 kHz | 4+ min | small table-driven step decode | specified only; `audio_start()` refuses it |

The current chant is 10.25 s = 82 KB = ~8 % of the chip, so compression was never needed.
`audio_start()` refusing format 1 is deliberate: refusing is better than playing noise.

Flash layout and the 16-byte header: [`audio-pipeline.md`](audio-pipeline.md).

## Low power

The sleep budget is the product. Order matters on the way down:

1. stop the sample timer, ramp the PWM back to mid-scale
2. PC3 LOW (amp → < 1 µA)
3. flash `B9h` deep power-down (→ < 1 µA)
4. MCU standby with EXTI on PD3 armed (→ ~9 µA total) — **not implemented**

Steps 1–3 are in `play_range()` today and run on every play. Step 4 is the open item: the
MCU currently polls PD3 in a `Delay_Ms(5)` loop, drawing a few mA. Skipping step 3 alone
would leave tens of µA of flash idle current and wreck the estimate, which is why it is
unconditional.

Any new feature must state its idle current.

## SPI flash commands used

| Cmd | Meaning | Notes |
|---|---|---|
| `9Fh` | JEDEC ID | must return `A1 40 14` — the one-shot proof all four SPI wires work |
| `03h` | read data | 3-byte big-endian address; ample for 64 kbit/s streaming |
| `0Bh` | fast read | defined in the header, unused — `03h` is fast enough at 3 MHz |
| `06h` | write enable | before any program/erase |
| `01h` | write status register | used by `spiflash_unprotect()` to clear the BP bits |
| `02h` | page program | 256 B page, must not cross a page boundary |
| `20h` / `D8h` | sector (4 KB) / block (64 KB) erase | erase before program; only `20h` is used |
| `C7h` | chip erase | implemented, unused by the tools |
| `05h` | read status register 1 | poll BUSY after every program/erase |
| `B9h` | deep power-down | last flash command before idling |
| `ABh` | release from deep power-down | first thing on wake; then wait ~20 µs |

## Source layout

```
src/board.h       pins and timing constants — SINGLE SOURCE OF PIN TRUTH
src/funconfig.h   ch32fun build config: 48 MHz, printf routing, SysTick divisor
src/main.c        init, boot diagnostics, play sequencing, switch polling
src/spiflash.c/.h SPI init and flash commands, plus the CHNT header reader
src/audio.c/.h    TIM1 PWM, TIM2 sample clock, double buffer, ramps, test tone
src/loader/       one-off loaders: loader_swio.c (SWIO, in use), loader.c (UART)
```

Never hard-code a port/pin outside `board.h`.

## Build order that mirrors bring-up

1. Blink/toggle PD2 once per second — proves power, solder, programmer, pin numbering.
2. Read SW1 on PD3 with the internal pull-up; toggle PD2 on each press; observe bounce.
3. Fixed 50 % PWM on PD2 — multimeter reads ≈1.5 V after R1, ≈0.16 V at the divider node.
4. SPI `9Fh` → expect `A1 40 14`; then write a page and read it back.
5. PC3 HIGH + a PWM tone → clean beep. (With PC3 LOW: silence.)
6. Stream the real chant from flash with the double buffer. ✅ **done**
7. Add standby/wake and the power-down sequencing; measure idle current. ⬜ **open**

[`bring-up.md`](bring-up.md) is the matching hardware checklist, with notes on what
actually happened at each stage.
