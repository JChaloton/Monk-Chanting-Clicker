# CLAUDE.md — chant-clicker

Guidance for Claude Code working in this repo. Read `docs/` before changing anything
electrical or timing-related; the numbers there were derived from datasheets and are
not to be "improved" casually.

## What this is

A ~30 mm keychain fidget clicker that plays a Buddhist monk chant when a blue
mechanical key switch is pressed. Built by March (Thailand) as a gift his mother will
give to a monk. It is also a deliberate learning project — March is doing the CAD,
circuit design, and firmware himself — and a possible small product later.

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
| 13 PC3 | amp enable, HIGH = play | U2 pin 4 SD (+ R5 100k pull-down) |
| 14 PC4 | SPI CS# | U3 pin 1 (+ R6 10k pull-up) |
| 15 PC5 | SPI SCK | U3 pin 6 CLK |
| 16 PC6 | SPI MOSI | U3 pin 5 DI |
| 17 PC7 | SPI MISO | U3 pin 2 DO |
| 4 PD7 | NRST | 10k pull-up (R4) |
| 2 PD5 / 3 PD6 | UART TX/RX | debug printf, and the audio loader |

## Firmware facts that constrain the code

- **No DAC.** Audio is 8-bit PWM on TIM1 CH1, 187.5 kHz carrier, **8 kHz sample rate**
  (one sample every 125 µs).
- **Audio lives in U3, never in MCU flash.** 16 KB of MCU flash is code only.
  1 MB ≈ 2 min raw 8-bit/8 kHz, 4+ min as 4-bit IMA ADPCM.
- **Double buffer, 2 × 256 B in RAM** (one flash page each). The PWM ISR plays buffer A
  while the main loop refills B over SPI. RAM is 2 KB total — budget accordingly.
- **Power discipline is the product.** Amp SD LOW and flash in deep power-down (`B9h`,
  wake `ABh`) whenever not playing; MCU in standby ≈ 9 µA. Skipping the flash
  power-down alone blows the whole sleep budget.
- **Debounce ~20 ms** on PD3. Ramp PWM duty at start and stop to avoid pops.
- Toolchain: **ch32v003fun**, driven either by the Makefiles or by PlatformIO
  (`platformio.ini`, community `platform-ch32v`). Programmer: WCH-LinkE.

## Repo layout

```
CLAUDE.md          this file
docs/              hardware, firmware plan, audio pipeline, bring-up, decisions
src/               player firmware — main.c, audio.c, spiflash.c
src/loader/        one-off UART→flash loader firmware (loader.c)
src/demo/          LED demo: the timing core on a bare dev board
tools/             send_chant.py (host → flash), wav2raw.py (stub), wav2led.py
scripts/           PlatformIO extra scripts
Makefile           the player      -> make flash
Makefile.loader    the loader      -> make -f Makefile.loader flash
Makefile.leddemo   the LED demo
platformio.ini     the same three firmwares as PlatformIO environments
```

**Two build systems, one set of sources.** The Makefiles are the reference;
`platformio.ini` builds the identical files out of `src/`. PlatformIO compiles
everything under `src/` by default and this tree holds three `main()`s, so each
environment carries an explicit `build_src_filter`. **Adding a .c file means editing
both** the matching Makefile's `ADDITIONAL_C_FILES` and that environment's filter —
otherwise one build system silently links a different program than the other.

## Working rules

1. **Datasheet wins.** Pin numbers and electrical limits come from the manufacturer
   datasheets only. Several hobby sites print a wrong PAM8302A pinout; the correct one
   is `1 NC, 2 IN+, 3 IN−, 4 SD, 5 VO+, 6 VDD, 7 GND, 8 VO−`. If a source disagrees
   with the datasheet, say so rather than silently averaging.
2. **`board.h` is the single source of pin truth.** Never hard-code a port/pin in a
   .c file. If a pin assignment changes, change it in `board.h` and in
   `docs/hardware.md` in the same commit.
3. **Don't break the sleep path.** Any new feature must answer: what is the current
   draw when idle? Adding a peripheral that idles at tens of µA defeats the design.
4. **Explain, don't just do.** March is learning the stack on this project. When
   writing firmware, briefly say why a register/approach was chosen — especially for
   timer, SPI, and low-power configuration. Keep it short; he has the background.
5. **Stage changes like bring-up.** Prefer small, individually testable steps that map
   onto `docs/bring-up.md`.
6. Comments and docs in English. Any user-facing product text may be Thai.

## Status

Hardware is built: everything is soldered onto perfboard around a CH32V003 dev board.

Firmware written and syntax-checked, **not yet run on hardware**:
- `src/spiflash.c` — full FM25Q08B driver
- `src/loader/loader.c` + `tools/send_chant.py` — UART → flash audio loader
- `src/audio.c`, `src/main.c` — the player

The load path was verified end to end against a Python model of the loader, so the
framing, checksums, padding and CRC agree on both sides. Nothing has touched a real
chip yet — work through `docs/bring-up.md` stages 4–6 in order.

Not implemented, on purpose: **step 7, MCU standby with EXTI wake.** The amp and the
flash are already powered down between plays; what is left is the MCU's poll loop, a
few mA. Fine on the bench, wrong for a keychain. See the header comment in `src/main.c`.

Open items are tracked in `docs/decisions.md`.
