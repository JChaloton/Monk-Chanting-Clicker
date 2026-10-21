# chant-clicker

A ~30 mm keychain clicker: press a blue mechanical key switch, hear a monk chant.
CH32V003 + PAM8302A + 1 MB SPI flash on one CR123A cell, designed to sleep at ~9 µA
between presses so a single cell lasts years.

Built as a gift for a Buddhist monk — and as a way to learn the whole stack: CAD,
circuit design, and firmware.

## Status — 2026-09-11

**It makes the right sound on real hardware.** Everything is on perfboard around a
CH32V003 dev board. Bring-up stages 1–6 pass: the flash answers `A1 40 14`, the 10.25 s
chant is in U3 with a CRC32 the MCU computed itself, and pressing SW1 plays it at the
right pitch and a useful volume.

What is left:

- **Stage 7 — MCU standby with EXTI wake is not implemented.** The amp and the flash
  already power down between plays (< 1 µA each), but the MCU still polls the switch, so
  bench idle current is a few mA, not µA. On a bench that is fine; on a keychain it is
  wrong. This is the last thing standing between the current firmware and the design's
  battery life. It was left out deliberately rather than guessed at — see the header
  comment in [`src/main.c`](src/main.c) and [`docs/decisions.md`](docs/decisions.md).
- **No PCB and no enclosure yet.** KiCad + JLCPCB when it happens; the CAD has not
  started. A CR123A is 17 × 34.5 mm, which is the real constraint on the 30 mm target.

Three bugs during first bring-up all presented as "no sound" and none were the same
problem. If anything regresses, read [`docs/debug-log.md`](docs/debug-log.md) before
re-deriving them.

## Docs

Start with the code tour if you are returning to this cold.

| | |
|---|---|
| [`docs/code-tour.md`](docs/code-tour.md) | **every file, what it does, why it is shaped that way** |
| [`docs/debug-log.md`](docs/debug-log.md) | the bugs that shaped the firmware, and how each was found |
| [`docs/hardware.md`](docs/hardware.md) | pin map, BOM, every part and why |
| [`docs/firmware-plan.md`](docs/firmware-plan.md) | the playback loop, the timer maths, the power sequence |
| [`docs/audio-pipeline.md`](docs/audio-pipeline.md) | chant → 8 kHz blob → flash, and how to swap the chant |
| [`docs/bring-up.md`](docs/bring-up.md) | staged assembly, pre-power checklist, what passed |
| [`docs/decisions.md`](docs/decisions.md) | what was decided and why, and what is still open |
| [`CLAUDE.md`](CLAUDE.md) | orientation + working rules for Claude Code |
| [`src/loader/README.md`](src/loader/README.md) | the UART→flash loader and its wire protocol |

Illustrated schematic walkthrough (block diagram + full drawn schematic):
https://claude.ai/code/artifact/a1a5eccc-82fa-48f8-8542-af769fb2e9c6

Datasheets live in the "monk chanting clicker" Claude project: CH32V003 (DS + RM),
PAM8302A, FM25Q08B.

## Build

Two ways, same sources. Pick one; they do not conflict.

**PlatformIO** — the route this project is built and flashed with:

```sh
pio run -e player -t upload       # the player
pio run -e loader -t upload       # the UART audio loader
pio device monitor                # player printf, PD5 @ 115200
```

Config is [`platformio.ini`](platformio.ini). The CH32V platform is community-maintained
and is fetched from git on the first build, which takes a few minutes once.

Which WCH-LinkE pin goes to which MCU pin, for both flashing and the audio loader, is in
[`docs/hardware.md`](docs/hardware.md#the-wch-linke--pins-and-how-to-hook-it-up). Three
things that account for most "it doesn't detect the chip": share a ground, put the dongle
in RISC-V mode (blue LED off when idle), and wire by the labels printed on it rather than
by pin position — the order moves between board revisions.

**Make** — the same two firmwares, kept as the reference build and as the cross-reference
for anything `docs/` describes in Makefile terms:

```sh
git submodule add https://github.com/cnlohr/ch32v003fun ch32v003fun
make            # build the player
make flash      # == pio run -e player -t upload
make monitor    # printf over the debug link (SWIO, not a COM port)
make load       # == python tools/load_audio.py   (see below)
```

On Windows `make flash` goes through `minichlink`, which needs a WinUSB driver binding
that PlatformIO's upload path handles for you — see [Windows: the driver](docs/hardware.md#windows-the-driver-and-why-platformio-is-the-easier-route).
**If `make flash` does not find the programmer, that is the known cause, and
`pio run -e player -t upload` is the way round it.** Both flash the same bytes over the
same SWIO wire.

Two firmwares live in one `src/` tree, so each environment names its own files in
`build_src_filter`. **Add a .c file and you must add it in two places** — the matching
filter and the Makefile — or the two builds quietly diverge.

Where `printf` comes out differs between the two backends, which is worth knowing before
you go hunting for a missing log: the **PlatformIO** `[env:player]` passes
`-DFUNCONF_USE_UARTPRINTF=1`, so printf lands on PD5 at 115200 and `pio device monitor`
works. The **Makefile** build uses ch32fun's default SWIO printf instead, read with
`make monitor` or `minichlink -T` — there is no COM port in that case. The loader never
uses either — it owns USART1 itself.

## Getting the chant into the flash chip

The loader and the player are two different firmwares and only one of them can be on the
MCU at a time, so a load is always three steps. One command does all three:

```sh
python tools/load_audio.py --pio                              # chant8k-limit.raw, port auto-detected
python tools/load_audio.py --pio other.raw --port COM9        # or say which
```

[`tools/load_audio.py`](tools/load_audio.py) runs loader → MCU, audio → SPI flash
(CRC-verified), player → MCU. **`--pio` is what picks the PlatformIO environments as the
build backend**; without it the script shells out to the Makefiles. Step 3 runs even if
step 2 fails either way, so the board never gets left holding the loader.

By hand, the three steps are three PlatformIO targets:

```sh
pio run -e loader -t upload                    # loader onto the MCU
pio run -e loader -t sendchant                 # CRC-verified; ~16 s for the 82 KB chant
pio run -e player -t upload                    # player back onto the MCU
```

`sendchant` is a custom target from [`scripts/pio_send_chant.py`](scripts/pio_send_chant.py)
that wraps `tools/send_chant.py`; the file, port and rate it uses are the
`custom_chant_*` options in `platformio.ini`'s `[env:loader]`. To pass a file or port
explicitly, call the tool directly — `python tools/send_chant.py COM7 chant.raw`.

<details>
<summary>The same three steps with Make</summary>

```sh
make load                                      # == python tools/load_audio.py
make load CHANT=other.raw PORT=COM9

make -f Makefile.loader flash                  # loader onto the MCU
python tools/send_chant.py COM7 chant.raw      # CRC-verified
make flash                                     # player back onto the MCU
```

</details>

Wiring for the middle step (labels are MCU-side, so TX/RX cross): adapter RX → PD5,
adapter TX → PD6, GND → GND. A WCH-LinkE doubles as the USB-serial adapter — its CDC
port is the one to use. Do not power the board from the adapter and the CR123A at once.

### Preparing the audio

```sh
ffmpeg -i chant.wav -ac 1 -ar 8000 -f u8 -acodec pcm_u8 chant.raw
python tools/raw_resample.py chant.raw chant8k-limit.raw --in-rate 8000 --limit 16
```

Two things to get right, because **nothing in the chain validates either of them**:

- **The rate.** A headerless `.raw` carries no sample rate; `send_chant.py` stamps
  whatever `--rate` says and the player believes it. Sanity check by size — the file
  should be **8000 bytes per second of audio**. The current chant is 10.25 s = 81 968 B.
  A 44.1 kHz file loaded as 8 kHz plays 5.5× slow, which sounds like *silence*, not like
  slow audio. That cost most of a session.
- **The loudness.** `--limit 16` is what makes it audible across a room. Peak
  normalisation alone leaves the chant ~13.7 dB below its own peaks and the analog chain
  is already flat out, so the headroom has to be reclaimed inside the file. Peaks land in
  the same place either way, so the amp never clips harder.

Details: [`docs/audio-pipeline.md`](docs/audio-pipeline.md) and
[`src/loader/README.md`](src/loader/README.md). With the flash on a DIP adapter instead,
a CH341A or DediProg writes `chant8k-limit.bin` directly and skips all of this.
