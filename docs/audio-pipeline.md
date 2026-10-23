# Audio pipeline — from a chant recording to bytes in U3

Four questions: what format, how loud, how the bytes get into the flash chip, and where
they sit once there.

## Format

Source: any chant recording (mom picks the chant). Convert to **mono, 8 kHz, 8-bit
unsigned PCM** — a raw headerless blob, no WAV header, because the firmware streams bytes
straight into the PWM compare register. Unsigned, because 128 is the PWM's idle
mid-point; signed samples play as loud distortion.

At 8 kHz / 8-bit, 1 MB holds **~2 minutes**. The chant that ships is 10.25 s = 81 968 B,
about 8 % of the chip, so capacity has never been a constraint. If a future chant is
longer than ~2 minutes, the header reserves format `1` for **4-bit IMA ADPCM** (~4+
minutes) — specified, but **not implemented on the MCU**; `audio_start()` refuses it
rather than playing noise.

Loudness: keep the samples at **full scale**. Attenuation is done in hardware by the
R2/R3 divider. Scaling down in software throws away resolution and adds hiss. High-pass
at ~200 Hz first — the 28 mm driver cannot reproduce below ~700 Hz anyway, and bass
content only eats headroom.

### Full scale is not the same as loud

This is where the volume actually lives, and it took a session to understand.

Peak normalisation sets the *peaks* to full scale and says nothing about the average.
`NaKomNamo.raw` peaks at 122/127 yet its RMS sits 13.7 dB below that, so the amp idles
through almost the entire chant. **There is no fix for this in the schematic**: R2/R3
already present ~0.32 Vpp against the PAM8302A's ~0.37 Vpp ceiling (see
[`hardware.md`](hardware.md)), so the analog chain is already flat out. The wasted
headroom is inside the file, and the only way to reclaim it is to shrink the crest
factor.

`raw_resample.py --limit` does exactly that. Peaks still land at `--headroom`, so **the
amp sees the same maximum input voltage and never clips harder** — only the gap between
peak and average closes. Measured on `NaKomNamo.raw`:

| flags | crest | vs peak-normalise alone |
|---|---|---|
| *(none)* | 13.7 dB | — |
| `--limit 16` | 10.3 dB | **+3.4 dB** |
| `--limit 16 --soft-clip 1.5` | 7.5 dB | **+6.2 dB** |

**`--limit 16`, no soft clip, is what ships** — confirmed by ear on the hardware. Reach
for `--limit` first: it only rescales the waveform, so it adds no distortion.
`--soft-clip` *bends* the waveform to buy the last few dB; 1.0–1.5 is inaudible on a
28 mm driver, 2 gets edgy, 3+ is obviously crunchy. Both default to off.

Two things that look like they should help and do not. Raising `--highpass` toward the
driver's 700 Hz corner *costs* loudness (−14.1 → −15.0 dBFS at 400 Hz) — the peaks in
this recording are not LF-driven, so removing in-band energy loses more than the headroom
it frees. And raising R3 to lift the divider ratio pushes the amp input past 0.37 Vpp
into clipping. Leave both alone.

### The rate is not checked anywhere — check it yourself

**A headerless `.raw` carries no sample rate.** `send_chant.py` writes whatever `--rate`
says (default 8000) straight into the header without looking at the audio, and the player
takes the header at its word. Nothing in the chain can detect a mismatch, so getting `-ar`
wrong is silent: the load succeeds, the CRC verifies, and the chant simply plays at the
wrong speed.

This cost most of a bring-up session. `NaKomNamoCompressed.raw` was really 44.1 kHz,
loaded as 8000, and played 5.5× slow — which sounded like *no* sound rather than slow
sound, because it dropped the voice's ~400 Hz fundamental to ~70 Hz, an octave and a half
below the 28 mm driver's 700 Hz corner. The firmware measured a perfect 8000 Hz sample
clock throughout; it was doing exactly as told. See [`debug-log.md`](debug-log.md).

Three ways to be sure of a file's real rate:

- `ffprobe chant.wav` before converting, and pass the same number to both `-ar` and
  `--rate`.
- **Check the size.** A correct file is **8000 bytes per second of audio**. 10.25 s =
  81 968 B. If it is 5× that, the export did not resample.
- For a `.raw` of unknown provenance, look at where its energy sits. Speech and chanting
  roll off around 3.5–4 kHz, so if 99 % of the energy is below ~0.16 of the file's
  Nyquist, the file is roughly 5× faster than you think it is.

`tools/raw_resample.py` converts a `.raw` whose real rate you now know, and does the
anti-alias low-pass, the 200 Hz high-pass and the normalisation in one pass:

```sh
python tools/raw_resample.py NaKomNamoCompressed.raw chant8k.raw --in-rate 44100
python tools/send_chant.py COM7 chant8k.raw --rate 8000
```

Do not "fix" a wrong-rate file by putting its native rate in the header instead. It
plays, but 44.1 kHz costs 5.5× the flash for detail the speaker cannot resolve — 1 MB
falls from ~2 minutes to ~23 seconds.

## Replacing the chant — the whole checklist

The player reads its sample rate from the flash header, so **swapping the chant is a
reload, not a firmware change.** `src/` is not touched at any point below.

1. **Export at 8 kHz.** Get the rate right here and the rest is bookkeeping:

   ```sh
   ffmpeg -i chant.wav -ac 1 -ar 8000 -f u8 -acodec pcm_u8 chant.raw
   ```

   `-ac 1` mono, `-ar 8000` the rate, `-f u8 -acodec pcm_u8` headerless unsigned 8-bit.
   Then check the size: **8000 bytes per second**. A 12-second chant is ~96 000 bytes. If
   it is 5× that, ffmpeg silently kept the source rate — which is exactly the bug that
   cost a session.

2. **Polish it.** ffmpeg does not do the 200 Hz high-pass, the normalisation, or the
   limiting, and all three matter on a 28 mm driver with no volume control:

   ```sh
   python tools/raw_resample.py chant.raw chant8k-limit.raw --in-rate 8000 --limit 16
   ```

   The same command handles a file exported at the wrong rate — pass its real rate as
   `--in-rate` and it resamples properly instead of aliasing.

   `--limit 16` is what makes it loud enough to hear across a room. Add
   `--soft-clip 1.5` for another ~2.8 dB if it is still too quiet, accepting some
   distortion. See **Format** above for why this cannot be fixed in hardware.

3. **Load it.** One command does all three firmware/transfer steps:

   ```sh
   python tools/load_audio_swio.py chant8k-limit.raw     # SWIO only, route D
   python tools/load_audio.py --pio chant8k-limit.raw    # UART on PD5/PD6, route C
   ```

   Use the first on any board that has nothing wired to PD5/PD6 — it needs only the
   WCH-LinkE that is already attached. The rest of this step describes the UART route;
   the SWIO one takes the same `.raw` and produces the same bytes in U3.

   `tools/load_audio.py` runs loader → MCU, audio → SPI flash, player → MCU; `--pio`
   selects the PlatformIO environments as the build backend. The port is auto-detected
   (pass `--port COM7` to be explicit). Step 3 runs even if step 2 fails, so the board is
   never left holding the loader.

   The three steps by hand are three PlatformIO targets. They take the file and port from
   `platformio.ini`'s `[env:loader]` — `custom_chant_file`, `custom_chant_port`,
   `custom_chant_args = --rate 8000`. **Those must agree with each other and with step 1;
   nothing validates them.**

   ```sh
   pio run -e loader -t upload
   pio run -e loader -t sendchant
   pio run -e player -t upload
   ```

   `sendchant` erases and rewrites from address 0, so the old chant needs no clearing.

   Make equivalent of the one-liner: `make load CHANT=chant8k-limit.raw`, with
   `PORT=COM7` to name the port.

4. **Check it played.** On the current three-wire board the simplest check is the one
   that matters: press the switch. The load already proved itself with a CRC32 the MCU
   computed over the whole image, which is a stronger check than reading a log.

   **The player's printf is not reachable on this board** — `[env:player]` sends it to
   PD5, which is not wired, and the SWIO alternative needs `minichlink -T`, which cannot
   open this probe. On a board that *does* wire PD5, `pio device monitor` at 115200
   prints the header it found and, after each play, the rate it measured:

   ```
   chant: fmt 0, 8000 Hz, 81968 bytes (~10 s)
   played 81968 of 81968 samples in 10246 ms = 7999 Hz (asked for 8000)
   ```

   If the byte count and duration match what you exported, it is right. `played` far
   short of the length means an underrun; `played 0` means the sample interrupt never
   fired. Without that log, the symptoms in [`bring-up.md`](bring-up.md)'s "If it goes
   wrong" table are what you have to go on.

Limits worth knowing before picking a chant: `spiflash_read_header()` accepts rates from
**2000 to 24000 Hz** and a length up to **1 MB minus the 16-byte header**, which is ~2
minutes at 8 kHz. Longer than that needs the ADPCM format, which is specified below but
not implemented.

## Flash layout

Simple and explicit, so the firmware never has to guess:

```
0x000000  16 bytes  header:  magic "CHNT" | format u8 | reserved u8 |
                             sample rate u32 LE | length u32 LE | reserved u16
0x000010  ...       audio data
```

Format byte: `0` = raw 8-bit unsigned PCM, `1` = 4-bit IMA ADPCM (not implemented).

The two u32 fields sit at offsets 6 and 10, which are **not 4-byte aligned**. The QingKe
V2A core does not do unaligned loads, so `spiflash_read_header()` assembles them a byte
at a time and nothing may cast that buffer to a `uint32_t *`.

A header costs nothing and means the player reads the length instead of having it
recompiled in. Keep the writer (`tools/`) and the reader (`src/spiflash.c`) in agreement
— if the header changes, change both in the same commit.

## Getting the bytes in — four routes

Every one of them writes U3 over SPI, because SPI is the only interface the chip has.
What differs is how the bytes get from the PC to the SPI bus: an external programmer
driving it directly (A, B), or the MCU driving it on the host's behalf (C, D). A and B
need the flash reachable by a clip; C and D work with it soldered down.

**A. CH341A USB programmer (simplest while the flash is on a DIP adapter).** Write the
chip on the desk with the adapter in the clip, then plug it into the board. Write
`chant8k-limit.bin` — the `.bin` already carries the 16-byte header, so it is a
byte-for-byte image of what belongs at address 0. Good for iterating on audio without
touching the MCU firmware.

**B. DediProg DP3000 at work.** March has access and has used MX25L series before. This
is the route for batch programming if the project ever becomes a small product — worth
confirming FM25Q08B is in the DediProg device list early.

**C. UART loader firmware — in-circuit, needs a serial adapter on PD5/PD6.** A one-off
firmware in `src/loader/` receives the blob over PD5 TX / PD6 RX and writes it to U3 page
by page. It is flashed *instead of* the player, so a load is always three steps:

```sh
python tools/load_audio.py --pio               # all three, auto-detected port
```

or by hand:

```sh
pio run -e loader -t upload
pio run -e loader -t sendchant
pio run -e player -t upload
```

With Make instead: `make load`, or `make -f Makefile.loader flash` /
`python tools/send_chant.py COM7 chant8k-limit.raw --rate 8000` / `make flash`.

The header labels are MCU-side, so TX and RX cross: adapter RX → PD5, adapter TX → PD6,
GND → GND. A WCH-LinkE is both the debug probe and the USB-serial adapter; use its CDC
port for this. **Do not power the board from the adapter and the CR123A at the same
time.**

Timing: 57600 baud, 261 bytes per page plus a status byte, so ~450 KB takes about 86 s
including erase and verification. The 82 KB chant takes about 16 s.

**D. SWIO loader firmware — in-circuit over the programming wire alone. The route for
boards with nothing on PD5/PD6.** Same idea as C and the same SPI writing underneath,
but the host reaches the MCU over the single SWIO wire (PD1) that the WCH-LinkE already
uses to program it. No serial adapter, no second cable, no UART pins required:

```sh
python tools/load_audio_swio.py                     # all three steps
python tools/load_audio_swio.py other.raw --rate 8000
```

Make equivalent: `make load-swio CHANT=other.raw`.

It flashes `src/loader/loader_swio.c`, pushes the image, verifies, and puts the player
back — the same three steps as C, and step 3 still runs if step 2 fails.

The transport is a **RAM mailbox**, not a byte stream. A debug probe can read and write
target memory: the host halts the core, drops a 512-byte request straight into the
loader's RAM at `0x20000300`, resumes, and the loader programs it out over SPI. No wire
protocol, nothing to resync. `tools/swio_link.py` is the host half; the mailbox layout
and command set are documented at the top of `src/loader/loader_swio.c` and the two must
change together.

### Backends — which tool drives the probe

`swio_link.py` can drive the WCH-LinkE two ways, and **which one works depends on the
driver bound to the probe**, not on preference:

| backend | per chunk | notes |
|---|---|---|
| `openocd` (default) | ~170 ms | one process stays up for the whole load, commands over its TCL port |
| `minichlink` | ~500 ms | one process per chunk, all of it USB re-enumeration |

**Measured end to end on hardware: 40 s** for the 82 KB chant on the OpenOCD backend —
3.5 s to flash the loader, ~30 s of transfer and CRC, 4.7 s to flash the player back.
That is ~1800 TCL round trips; the SPI writing itself is milliseconds.

**On this machine only OpenOCD works.** The WCH-LinkRV interface is bound to WCH's own
driver, which OpenOCD and `wlink` talk to; minichlink wants a WinUSB binding and fails
with `libusb_open() = -5`. That is the same thing that makes `make flash` fail — see
[`hardware.md`](hardware.md). OpenOCD comes from PlatformIO's `tool-openocd-riscv-wch`,
which is already installed because `pio run -t upload` uses it.

`--backend minichlink` forces the other one, `CHANT_OPENOCD` / `CHANT_MINICHLINK`
override the paths, and `auto` prefers OpenOCD and says so if it falls back.

The host reads an acknowledgement after every chunk on the OpenOCD backend, and this is
**required for correctness, not a safety belt**. What matters between two chunks is not
elapsed time but how long the core is actually *running*, and nearly all of an OpenOCD
chunk is spent halted: halt, write the scratch file, `load_image`, resume — the loader
only executes between that resume and the next halt, a few milliseconds against up to
6 ms of page programming. Skipping the ack here produces a clean-looking run that fails
the final CRC32. See [`debug-log.md`](debug-log.md) #7.

On minichlink the ack is skipped, safely, for the opposite reason: the core runs for the
whole ~250 ms of the next process's startup and USB enumeration. Either way sector
erases, which take up to 300 ms, are waited on, and a CRC32 over the whole image at the
end catches anything that slipped. `--verify-each` forces the ack on both.

One thing that looks like it should work and does not: **ch32fun's SWIO printf channel**.
It moves three bytes per USB round trip — tens of thousands of transactions for 82 KB —
and on Windows minichlink reads the host end of it with `_kbhit()`/`_getch()`, which only
see a real console. Nothing can pipe into it from a script.

**D needs no pyserial and no COM port.**

The wire protocol for route C lives in [`../src/loader/README.md`](../src/loader/README.md). In
short: one ASCII byte per command, every reply starting with a status byte, fixed
256-byte pages with a checksum, and a CRC32 the MCU computes over the whole image at the
end. The firmware does the usual dance internally — `06h` write enable before each page
program, `20h` sector erase before programming, `05h` BUSY polled after every write — so
none of that leaks into the protocol.

## Verification

Always verify before trusting a load — a silent bad byte in the middle of a chant is hard
to diagnose by ear. `send_chant.py` does it by default: the MCU computes a CRC32 over
everything it wrote and sends four bytes back, and the host compares that against
`zlib.crc32` of the same padded image. Reading the whole megabyte back over the UART
would have cost as long again as writing it.

`--no-verify` skips it. There is no good reason to.

## The files on disk

`.gitignore` excludes `*.raw` and `*.wav`, so none of these are in git.

| File | Size | Really is | What it is |
|---|---|---|---|
| `NaKomNamoCompressed.raw` | 451 849 B | 10.25 s @ **44.1 kHz** | the original export, and the file that caused the wrong-rate bug |
| `NaKomNamo.raw` | 81 968 B | 10.25 s @ 8 kHz | 8 kHz export, crest 13.7 dB — the "quiet" baseline |
| `chant8k.raw` | 81 968 B | 10.25 s @ 8 kHz | `raw_resample.py` without the limiter, crest 14.0 dB |
| `chant8k-limit.raw` | 81 968 B | 10.25 s @ 8 kHz | **ships.** `--limit 16`, crest 10.3 dB |
| `chant8k-limit.bin` | 81 984 B | — | the same with the `CHNT` header — a direct image for a CH341A/DediProg |
