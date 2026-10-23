# chant-clicker

A ~30 mm keychain clicker: press a blue mechanical key switch, hear a monk chant.
CH32V003 + PAM8302A + 1 MB SPI flash on one CR123A cell, designed to sleep at ~9 µA
between presses so a single cell lasts years.

Built as a gift for a Buddhist monk — and as a way to learn the whole stack: CAD,
circuit design, and firmware.

## Status — 2026-10-23

**A board is programmed end to end over a single signal wire.** Connect the WCH-LinkE's
SWIO, GND and 3V3, run one command, press the switch:

```sh
python tools/load_audio_swio.py
```

~40 s. It flashes a loader, pushes the 82 KB chant into the SPI flash over the debug
link, verifies it with a CRC32 the MCU computes itself, and puts the player back. No
serial adapter, no second cable, nothing on PD5/PD6 — which the current board revision
does not wire at all.

Before this, loading a chant needed a USB-serial adapter on PD5/PD6 as well as the
programmer. That route ([`src/loader/loader.c`](src/loader/loader.c)) still works and is
still in the tree; it is simply no longer what this board needs.

Also true since the last status: the clicker makes the right sound on real hardware,
bring-up stages 1–6 pass, the flash answers `A1 40 14`, and pressing SW1 plays the
10.25 s chant at the right pitch and a useful volume.

What is left:

- **Stage 7 — MCU standby with EXTI wake is not implemented.** The amp and the flash
  already power down between plays (< 1 µA each), but the MCU still polls the switch, so
  bench idle current is a few mA, not µA. On a bench that is fine; on a keychain it is
  wrong. This is the last thing standing between the current firmware and the design's
  battery life. It was left out deliberately rather than guessed at — see the header
  comment in [`src/main.c`](src/main.c) and [`docs/decisions.md`](docs/decisions.md).
- **No verified way to read the player's `printf` on a three-wire board.** The PlatformIO
  player sends it to PD5, which is no longer wired; the SWIO alternative is
  `minichlink -T`, and minichlink cannot open this probe. `wlink sdi-print` is the
  untested candidate. See [`docs/decisions.md`](docs/decisions.md).
- **No PCB and no enclosure yet.** KiCad + JLCPCB when it happens; the CAD has not
  started. A CR123A is 17 × 34.5 mm, which is the real constraint on the 30 mm target.

Seven bugs are written up in [`docs/debug-log.md`](docs/debug-log.md). Read it before
re-deriving any of them — most presented as "no sound", and the two newest are about the
programming link rather than the audio.

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
| [`src/loader/README.md`](src/loader/README.md) | both loader firmwares — SWIO and UART — and the UART wire protocol |

Illustrated schematic walkthrough (block diagram + full drawn schematic):
https://claude.ai/code/artifact/a1a5eccc-82fa-48f8-8542-af769fb2e9c6

Datasheets live in the "monk chanting clicker" Claude project: CH32V003 (DS + RM),
PAM8302A, FM25Q08B.

## Build

Two ways, same sources. Pick one; they do not conflict.

**PlatformIO** — the route this project is built and flashed with:

```sh
pio run -e player -t upload       # the player
pio run -e loader_swio -t upload  # the SWIO audio loader  (route D, the one in use)
pio run -e loader -t upload       # the UART audio loader  (route C, needs PD5/PD6)
```

Config is [`platformio.ini`](platformio.ini). The CH32V platform is community-maintained
and is fetched from git on the first build, which takes a few minutes once.

Which WCH-LinkE pin goes to which MCU pin — the same three for flashing *and* for
loading the chant — is in
[`docs/hardware.md`](docs/hardware.md#the-wch-linke--pins-and-how-to-hook-it-up). Three
things account for most "it doesn't detect the chip": share a ground, put the dongle in
RISC-V mode (blue LED off when idle), and wire by the labels printed on it rather than
by pin position — the order moves between board revisions. A fourth, specific to this
machine: **`minichlink` cannot open this probe at all** (`libusb_open() = -5`); OpenOCD
and `wlink` can. Never reach for a `minichlink` command here — see
[`docs/debug-log.md`](docs/debug-log.md) #6.

**Make** — the same three firmwares, kept as the reference build and as the cross-reference
for anything `docs/` describes in Makefile terms:

```sh
git submodule add https://github.com/cnlohr/ch32v003fun ch32v003fun
make            # build the player
make flash      # == pio run -e player -t upload
make load-swio  # == python tools/load_audio_swio.py   (three wires, see below)
make load       # == python tools/load_audio.py        (route C, needs PD5/PD6)
```

On Windows `make flash` goes through `minichlink`, which needs a WinUSB driver binding
that PlatformIO's upload path handles for you — see [Windows: the driver](docs/hardware.md#windows-the-driver-and-why-platformio-is-the-easier-route).
**If `make flash` does not find the programmer, that is the known cause, and
`pio run -e player -t upload` is the way round it.** Both flash the same bytes over the
same SWIO wire.

Three firmwares live in one `src/` tree — player, `loader_swio`, `loader` — so each
environment names its own files in `build_src_filter`, and there is one Makefile per
image (`Makefile`, `Makefile.loader-swio`, `Makefile.loader`). **Add a .c file and you
must add it in two places** — the matching filter and the matching Makefile — or the two
build systems quietly diverge.

Where `printf` comes out differs between the two backends: the **PlatformIO**
`[env:player]` passes `-DFUNCONF_USE_UARTPRINTF=1`, so printf lands on PD5 at 115200 and
`pio device monitor` reads it. The **Makefile** build uses ch32fun's default SWIO printf,
read with `minichlink -T`. Neither loader uses either — `loader.c` owns USART1 itself and
`loader_swio.c` builds with printf compiled out, so it cannot contend with the debug
channel its host is driving.

**On the current board neither printf route works**: PD5 is not wired, and minichlink
cannot open this probe. That is an open item, not a solved one — see the status above.

## Getting the chant into the flash chip

The loader and the player are two different firmwares and only one of them can be on the
MCU at a time, so a load is always three steps. **One command does all three, over the
three wires that already program the board:**

```sh
python tools/load_audio_swio.py                   # chant8k-limit.raw
python tools/load_audio_swio.py other.raw --rate 8000
```

[`tools/load_audio_swio.py`](tools/load_audio_swio.py) flashes
[`src/loader/loader_swio.c`](src/loader/loader_swio.c), pushes the audio into U3 through
a RAM mailbox over the debug link, checks a CRC32 the MCU computed over the whole image,
and puts the player back. ~40 s for the 82 KB chant. Step 3 runs even if step 2 fails,
so the board never gets left holding the loader. Make equivalent: `make load-swio`.

Needs no pyserial and no COM port. It does need a debug tool that can open the probe;
`swio_link.py` prefers PlatformIO's OpenOCD, which is the one that works here, and
`--backend minichlink` forces the other.

If a run ever stalls, run it again — it erases and rewrites from address 0, so a partial
load leaves nothing to clean up.

<details>
<summary>Route C — the older UART loader, for a board that wires PD5/PD6</summary>

```sh
python tools/load_audio.py --pio                              # port auto-detected
python tools/load_audio.py --pio other.raw --port COM9        # or say which
```

By hand, the three steps are three PlatformIO targets:

```sh
pio run -e loader -t upload                    # loader onto the MCU
pio run -e loader -t sendchant                 # CRC-verified; ~16 s for the 82 KB chant
pio run -e player -t upload                    # player back onto the MCU
```

`sendchant` is a custom target from [`scripts/pio_send_chant.py`](scripts/pio_send_chant.py)
that wraps `tools/send_chant.py`; the file, port and rate it uses are the
`custom_chant_*` options in `platformio.ini`'s `[env:loader]`.

With Make: `make load`, or `make -f Makefile.loader flash` /
`python tools/send_chant.py COM7 chant.raw` / `make flash`.

Wiring for the middle step (labels are MCU-side, so TX/RX cross): adapter RX → PD5,
adapter TX → PD6, GND → GND. A WCH-LinkE doubles as the USB-serial adapter — its CDC
port is the one to use. Do not power the board from the adapter and the CR123A at once.

</details>

### Preparing the audio

```sh
ffmpeg -i chant.wav -ac 1 -ar 8000 -f u8 -acodec pcm_u8 chant.raw
python tools/raw_resample.py chant.raw chant8k-limit.raw --in-rate 8000 --limit 16
```

Two things to get right, because **nothing in the chain validates either of them**:

- **The rate.** A headerless `.raw` carries no sample rate; the loader tool stamps
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
