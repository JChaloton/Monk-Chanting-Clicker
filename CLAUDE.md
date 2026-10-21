# CLAUDE.md — chant-clicker

Guidance for Claude Code working in this repo. Read `docs/` before changing anything
electrical or timing-related; the numbers there were derived from datasheets and
measured on hardware, and are not to be "improved" casually.

Fastest orientation: [`docs/code-tour.md`](docs/code-tour.md) (every file, and why it is
shaped that way), then [`docs/debug-log.md`](docs/debug-log.md) (the bugs that shaped it).

## What this is

A ~30 mm keychain fidget clicker that plays a Buddhist monk chant when a blue mechanical
key switch is pressed. Built by March (Thailand) as a gift his mother will give to a
monk. It is also a deliberate learning project — March is doing the CAD, circuit design,
and firmware himself — and a possible small product later.

Design intent that follows from that:

- The switch does **not** gate power or audio. It only signals the MCU; software does
  debounce, full-chant playback, amp shutdown, and sleep. Do not "simplify" this.
- Battery is a replaceable primary cell (CR123A). Rechargeable Li-ion was ruled out on
  cost (LIR2450 ≈ 150–200 THB in Thailand). No charger circuit, no regulator.
- Cost matters. Parts are sourced from LCSC, boards from JLCPCB.

## Hardware, in one screen

Single 3 V rail from a CR123A, no regulator. Full detail: `docs/hardware.md`.

| Ref | Part | Package |
|---|---|---|
| U1 | CH32V003F4P6 RISC-V MCU, 16 KB flash / 2 KB RAM, 2.7–5.5 V | TSSOP-20 (0.65 mm) |
| U2 | PAM8302AADCR mono class-D amp, 2.0–5.5 V, ~24 dB fixed gain, active-LOW SD | MSOP-8 (0.65 mm) |
| U3 | FM25Q08B SPI NOR flash, 8 Mbit = 1 MB, 2.3–3.6 V, JEDEC ID `A1 40 14` | SOP-8 (1.27 mm) |
| LS1 | HDK-2808BB-7, 28 mm, 8 Ω, 1.5 W, 700 Hz–5 kHz, 97 dB | — |
| SW1 | blue (clicky) mechanical key switch | — |
| BT1 | CR123A, 3 V lithium primary, ~1500 mAh | — |

Pin map (this is the contract between `board.h` and the schematic):

| U1 pin | Signal | Goes to |
|---|---|---|
| 9 VDD / 7 VSS | power | 3 V rail / GND |
| 18 PD1 | SWIO | J1 programming header (WCH-LinkE) — keep dedicated |
| 19 PD2 | audio PWM, TIM1 CH1 | R1 → C1 low-pass → R2/R3 divider → C2 → U2 IN+ |
| 20 PD3 | switch input, active-LOW, internal pull-up, EXTI wake | SW1 → GND |
| 13 PC3 | amp enable, HIGH = play | U2 pin 4 SD (+ R4 100k pull-down) |
| 14 PC4 | SPI CS# | U3 pin 1 (+ 10k pull-up) |
| 15 PC5 | SPI SCK | U3 pin 6 CLK |
| 16 PC6 | SPI MOSI | U3 pin 5 DI |
| 17 PC7 | SPI MISO | U3 pin 2 DO |
| 4 PD7 | NRST | 10k pull-up (R5) |
| 2 PD5 / 3 PD6 | UART TX/RX | debug printf, and the audio loader |

Reference designators: **R4 = 100k SD pull-down at the amp, R5 = 10k NRST pull-up.**
`docs/hardware.md`'s passives table is authoritative if anything ever disagrees.

## Firmware facts that constrain the code

- **No DAC.** Audio is 8-bit PWM on TIM1 CH1, 187.5 kHz carrier, **8 kHz sample rate**
  (one sample every 125 µs).
- **The sample tick is TIM2**, `ATRLR = 5999` (48 MHz / 8000 = 6000 exactly). It is
  **not** SysTick and must never become SysTick again: ch32fun builds `Delay_Us()` /
  `Delay_Ms()` on the free-running SysTick counter, and taking it over hangs the
  firmware inside `audio_stop()` with the amp still on. See `docs/debug-log.md`.
- **The sample rate comes from the flash header**, not from `board.h`. `board.h`'s 8000
  is only the default the host tools stamp. Swapping the chant is a reload, not a
  firmware change.
- **Audio lives in U3, never in MCU flash.** 16 KB of MCU flash is code only.
  1 MB ≈ 2 min raw 8-bit/8 kHz. The current chant is 10.25 s = 82 KB, ~8 % of the chip.
- **Double buffer, 2 × 256 B in RAM** (one flash page each). The TIM2 ISR plays buffer A
  while the main loop refills B over SPI. **The ISR must never touch SPI.** RAM is 2 KB
  total — budget accordingly.
- **Power discipline is the product.** Amp SD LOW and flash in deep power-down (`B9h`,
  wake `ABh`) whenever not playing; MCU standby ≈ 9 µA. Skipping the flash power-down
  alone blows the whole sleep budget.
- **Debounce ~20 ms** on PD3. Ramp PWM duty at start and stop to avoid pops.
- Toolchain: **ch32v003fun** (note: upstream renamed its header to `ch32fun.h` and its
  makefile fragment to `ch32fun/ch32fun.mk`), driven either by PlatformIO
  (`platformio.ini`, community `platform-ch32v`) or by the Makefiles. Programmer:
  WCH-LinkE. **Flashing here is done with PlatformIO** — `pio run -e player -t upload`,
  `pio run -e loader -t upload`, `python tools/load_audio.py --pio` — because `make flash`
  does not work on this machine's Windows setup (`minichlink` WinUSB binding; see
  `docs/hardware.md`). Lead with the `pio` commands; give the `make` ones only as the
  cross-reference.

## Repo layout

```
CLAUDE.md          this file
docs/              code tour, debug log, hardware, firmware, audio pipeline, bring-up, decisions
src/               player firmware — main.c, audio.c, spiflash.c
src/loader/        one-off UART→flash loader firmware (loader.c)
tools/             load_audio.py (do-it-all), send_chant.py, raw_resample.py,
                   wav2raw.py (stub)
scripts/           PlatformIO extra scripts
platformio.ini     the two firmwares as PlatformIO environments -> pio run -e player|loader -t upload
Makefile           the player      -> make flash   /  make load      (reference build)
Makefile.loader    the loader      -> make -f Makefile.loader flash
```

**Two build systems, one set of sources.** `platformio.ini` is the one in use here; the
Makefiles are the reference and build the identical files out of `src/`. PlatformIO compiles everything
under `src/` by default and this tree holds two `main()`s, so each environment carries
an explicit `build_src_filter`. **Adding a .c file means editing both** the matching
Makefile's `ADDITIONAL_C_FILES` and that environment's filter — otherwise one build
system silently links a different program than the other.

## Working rules

1. **Datasheet wins.** Pin numbers and electrical limits come from the manufacturer
   datasheets only. Several hobby sites print a wrong PAM8302A pinout; the correct one
   is `1 NC, 2 IN+, 3 IN−, 4 SD, 5 VO+, 6 VDD, 7 GND, 8 VO−`. If a source disagrees with
   the datasheet, say so rather than silently averaging.
2. **`board.h` is the single source of pin truth.** Never hard-code a port/pin in a .c
   file. If a pin assignment changes, change it in `board.h` and in `docs/hardware.md` in
   the same commit.
3. **Don't break the sleep path.** Any new feature must answer: what is the current draw
   when idle? Adding a peripheral that idles at tens of µA defeats the design.
4. **Don't break the timebase.** SysTick belongs to ch32fun's delays and to
   `play_range()`'s independent rate measurement. The sample clock is TIM2.
5. **Explain, don't just do.** March is learning the stack on this project. When writing
   firmware, briefly say why a register/approach was chosen — especially for timer, SPI,
   and low-power configuration. Keep it short; he has the background.
6. **Stage changes like bring-up.** Prefer small, individually testable steps that map
   onto `docs/bring-up.md`.
7. Comments and docs in English. Any user-facing product text may be Thai.

## Status — 2026-09-11

**Working on hardware.** Perfboard build around a CH32V003 dev board; bring-up stages
1–6 pass. The flash reads `A1 40 14`, `chant8k-limit.raw` (10.25 s, 8 kHz, 82 KB) is in
U3 with a matching MCU-computed CRC32, and a press plays it at the right pitch and a
useful volume.

Three bugs found during that session, all fixed, all documented in `docs/debug-log.md`:
the host reconfiguring the serial port mid-transaction, an unbounded loader read turning
one dropped byte into spurious sector erases, and a 44.1 kHz chant loaded as 8 kHz. Plus
the SysTick→TIM2 move above.

Not implemented, on purpose: **step 7, MCU standby with EXTI wake.** The amp and the
flash are already powered down between plays; what is left is the MCU's poll loop, a few
mA. Fine on the bench, wrong for a keychain. See the header comment in `src/main.c`.

Open items are tracked in `docs/decisions.md`.
