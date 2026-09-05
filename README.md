# chant-clicker

A ~30 mm keychain clicker: press a blue mechanical key switch, hear a monk chant.
CH32V003 + PAM8302A + 1 MB SPI flash on one CR123A cell, sleeping at ~9 µA between
presses so a single cell lasts years.

Built as a gift for a Buddhist monk — and as a way to learn the whole stack: CAD,
circuit design, and firmware.

## Status

Design settled on paper. Nothing built, no firmware written. `src/` is a skeleton.

Next hardware step: breadboard bring-up, `docs/bring-up.md` (needs a WCH-LinkE).
Next firmware step: toggle PD2 under the programmer — stage 2.

## Docs

| | |
|---|---|
| [`CLAUDE.md`](CLAUDE.md) | orientation + working rules for Claude Code |
| [`docs/hardware.md`](docs/hardware.md) | pin map, BOM, every part and why |
| [`docs/firmware-plan.md`](docs/firmware-plan.md) | playback loop, double buffer, power discipline |
| [`docs/audio-pipeline.md`](docs/audio-pipeline.md) | chant → 8 kHz blob → flash |
| [`docs/bring-up.md`](docs/bring-up.md) | staged assembly + pre-power checklist |
| [`docs/decisions.md`](docs/decisions.md) | what was decided and why, and what is still open |

Illustrated schematic walkthrough (block diagram + full drawn schematic):
https://claude.ai/code/artifact/a1a5eccc-82fa-48f8-8542-af769fb2e9c6

Datasheets live in the "monk chanting clicker" Claude project: CH32V003 (DS + RM),
PAM8302A, FM25Q08B.

## Build

```sh
git submodule add https://github.com/cnlohr/ch32v003fun ch32v003fun
make            # build
make flash      # WCH-LinkE over SWIO (PD1)
make monitor    # printf over PD5
```

## Audio

```sh
tools/wav2raw.py chant.wav chant.bin     # mono, 8 kHz, 8-bit + 16-byte header
```

Then get `chant.bin` into U3 by CH341A, DediProg, or the UART loader — see
`docs/audio-pipeline.md`.
