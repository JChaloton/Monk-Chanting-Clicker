# chant-clicker

A ~30 mm keychain clicker: press a blue mechanical key switch, hear a monk chant.
CH32V003 + PAM8302A + 1 MB SPI flash on one CR123A cell, sleeping at ~9 µA between
presses so a single cell lasts years.

Built as a gift for a Buddhist monk — and as a way to learn the whole stack: CAD,
circuit design, and firmware.

## Status

Built on perfboard around a CH32V003 dev board. Player firmware, the UART audio
loader, and the host uploader are written; none of it has run on the hardware yet.
Work through `docs/bring-up.md` stages 4–6 in order.

MCU standby (stage 7) is deliberately not implemented yet — the amp and flash already
power down between plays, but the MCU still polls, so bench idle current is mA, not µA.

## Docs

| | |
|---|---|
| [`CLAUDE.md`](CLAUDE.md) | orientation + working rules for Claude Code |
| [`docs/hardware.md`](docs/hardware.md) | pin map, BOM, every part and why |
| [`docs/firmware-plan.md`](docs/firmware-plan.md) | playback loop, double buffer, power discipline |
| [`docs/audio-pipeline.md`](docs/audio-pipeline.md) | chant → 8 kHz blob → flash |
| [`docs/bring-up.md`](docs/bring-up.md) | staged assembly + pre-power checklist |
| [`docs/decisions.md`](docs/decisions.md) | what was decided and why, and what is still open |
| [`docs/led-demo.md`](docs/led-demo.md) | run the player on a bare dev board, output to an LED |
| [`src/loader/README.md`](src/loader/README.md) | the UART→flash loader and its wire protocol |

Illustrated schematic walkthrough (block diagram + full drawn schematic):
https://claude.ai/code/artifact/a1a5eccc-82fa-48f8-8542-af769fb2e9c6

Datasheets live in the "monk chanting clicker" Claude project: CH32V003 (DS + RM),
PAM8302A, FM25Q08B.

## Build

Two ways, same sources. Pick one; they do not conflict.

**Make** — the reference build:

```sh
git submodule add https://github.com/cnlohr/ch32v003fun ch32v003fun
make            # build the player
make flash      # WCH-LinkE over SWIO (PD1)
make monitor    # printf over the debug link
```

**PlatformIO** — `platformio.ini`, for editor integration and one-click upload.
The CH32V platform is community-maintained and is fetched from git on the first
build, which takes a few minutes once:

```sh
pio run -e player  -t upload      # the player
pio run -e loader  -t upload      # the UART audio loader
pio run -e leddemo -t upload      # the LED demo
```

Three firmwares live in one `src/` tree, so each environment names its own files
in `build_src_filter`. Add a .c file and you must add it in two places — the
Makefile and the matching filter — or the two builds quietly diverge.

The player's `printf` goes over SWIO, not a COM port, so `pio device monitor` will
not show it; use `make monitor` or `minichlink -T`.

## Getting the chant into the flash chip

Convert the recording, then push it through the MCU into U3. Three commands, in
this order — the loader and the player are two different firmwares and only one of
them can be on the chip at a time.

```sh
ffmpeg -i chant.wav -ac 1 -ar 8000 -f u8 -acodec pcm_u8 chant.raw

make -f Makefile.loader flash                  # loader onto the MCU
python tools/send_chant.py COM7 chant.raw      # ~2 min for a full 1 MB, CRC-verified
make flash                                     # player back onto the MCU
```

Wiring for the middle step (labels are MCU-side, so TX/RX cross): adapter RX → PD5,
adapter TX → PD6, GND → GND. A WCH-LinkE doubles as the USB-serial adapter. Do not
power the board from the adapter and the CR123A at once.

Details and the wire protocol: [`src/loader/README.md`](src/loader/README.md) and
[`docs/audio-pipeline.md`](docs/audio-pipeline.md). With the flash on a DIP adapter
instead, a CH341A or DediProg writes `chant.raw` directly and skips all of this.

No amp or flash chip yet? The same recording can drive an LED on a bare dev board,
which exercises the PWM and the 8 kHz sample clock without either of them:

```sh
tools/wav2led.py chant.wav src/demo/chant_led.h   # -> C array in MCU flash
make -f Makefile.leddemo flash
```

See `docs/led-demo.md` for wiring and the step-by-step.
