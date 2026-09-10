# Audio pipeline — from a chant recording to bytes in U3

Three questions: what format, how the bytes get into the flash chip, and where they sit.

## Format

Source: any chant recording (mom picks the chant). Convert to **mono, 8 kHz, 8-bit
unsigned PCM** — a raw headerless blob, no WAV header, because the firmware streams
bytes straight into the PWM compare register.

At 8 kHz / 8-bit, 1 MB holds **~2 minutes**. If the chant is longer, switch to **4-bit
IMA ADPCM** (~4+ minutes) and decode on the MCU — a small table-driven step decoder,
easily within the CH32V003's budget.

Loudness: keep the samples at **full scale**. Attenuation is done in hardware by the
R2/R3 divider. Scaling down in software throws away resolution and adds hiss. Normalise
to just under clipping, and high-pass at ~200 Hz first — the 28 mm driver cannot
reproduce below ~700 Hz anyway, and bass content only eats headroom.

**Full scale is not the same as loud, and this is where the volume actually lives.**
Peak normalisation sets the *peaks* to full scale and says nothing about the average.
`NaKomNamo.raw` peaks at 122/127 yet averages 14 dB below that, so the amp idles
through almost the whole chant. There is no fix for this in the schematic: R2/R3
already present ~0.32 Vpp against the PAM8302A's ~0.37 Vpp ceiling (see
`docs/hardware.md`), so the analog chain is already flat out. The wasted headroom is
inside the file, and the only way to reclaim it is to shrink the crest factor.

`raw_resample.py --limit` does that. Peaks still land at `--headroom`, so **the amp
sees exactly the same maximum input voltage and never clips harder** — only the gap
between peak and average closes. On `NaKomNamo.raw`:

| flags | crest | vs peak-normalise alone |
|---|---|---|
| *(none)* | 13.7 dB | — |
| `--limit 16` | 10.3 dB | **+3.9 dB** |
| `--limit 16 --soft-clip 1.5` | 7.5 dB | **+6.6 dB** |

Reach for `--limit` first — it only rescales the waveform, so it adds no distortion.
`--soft-clip` bends the waveform to buy the last few dB; 1.0–1.5 is inaudible on a
28 mm driver, 2 gets edgy, 3+ is obviously crunchy. Both default to off.

Two things that look like they should help and do not. Raising `--highpass` toward the
driver's 700 Hz corner *costs* loudness (−14.1 → −15.0 dBFS at 400 Hz) — the peaks in
this recording are not LF-driven, so removing in-band energy loses more than the
headroom it frees. And raising R3 to lift the divider ratio pushes the amp input past
0.37 Vpp into clipping. Leave both alone.

`tools/wav2raw.py` is still a stub. Until it exists, ffmpeg does the same conversion:

```sh
ffmpeg -i chant.wav -ac 1 -ar 8000 -f u8 -acodec pcm_u8 chant.raw
```

What ffmpeg will not do is the ~200 Hz high-pass and the peak normalisation, which is
the reason to finish `wav2raw.py` eventually. `tools/send_chant.py` writes the 16-byte
header itself, so a headerless `.raw` from ffmpeg is directly loadable.

### The rate is not checked anywhere — check it yourself

**A headerless `.raw` carries no sample rate.** `send_chant.py` writes whatever
`--rate` says (default 8000) straight into the header without looking at the audio,
and the player takes the header at its word. Nothing in the chain can detect a
mismatch, so getting `-ar` wrong is silent — the load succeeds, the CRC verifies, and
the chant simply plays at the wrong speed.

This cost most of a bring-up session. `NaKomNamoCompressed.raw` was really 44.1 kHz,
loaded as 8000, and played 5.5x slow — which sounded like *no* sound rather than slow
sound, because it dropped the voice's ~400 Hz fundamental to ~70 Hz, an octave and a
half below the 28 mm driver's 700 Hz corner. The firmware measured a perfect 8000 Hz
sample clock throughout; it was doing exactly as told.

Two ways to be sure of a file's real rate:

- `ffprobe chant.wav` before converting, and pass the same number to both `-ar` and
  `--rate`.
- For a `.raw` of unknown provenance, look at where its energy sits. Speech and
  chanting roll off around 3.5–4 kHz, so if 99 % of the energy is below ~0.16 of the
  file's Nyquist, the file is roughly 5x faster than you think it is.

`tools/raw_resample.py` converts a `.raw` whose real rate you now know, and does the
anti-alias low-pass, the 200 Hz high-pass and the normalisation in one pass:

```sh
python tools/raw_resample.py NaKomNamoCompressed.raw chant8k.raw --in-rate 44100
python tools/send_chant.py COM7 chant8k.raw --rate 8000
```

Do not "fix" a wrong-rate file by putting its native rate in the header instead. It
plays, but 44.1 kHz costs 5.5x the flash for audio the speaker cannot resolve — 1 MB
falls from ~2 minutes to ~23 seconds.

## Replacing the chant — the whole checklist

The player reads its sample rate from the flash header, so **swapping the chant is a
reload, not a firmware change.** `src/` is not touched at any point below.

1. **Export at 8 kHz.** Get the rate right here and the rest is bookkeeping:

   ```sh
   ffmpeg -i chant.wav -ac 1 -ar 8000 -f u8 -acodec pcm_u8 chant.raw
   ```

   `-ac 1` mono, `-ar 8000` the rate, `-f u8 -acodec pcm_u8` headerless unsigned
   8-bit. Confirm what you actually got — `ffmpeg` will silently keep the source rate
   if the argument lands in the wrong place, which is exactly the bug that cost a
   session:

   ```sh
   ffprobe -v error -show_entries stream=sample_rate,channels,duration chant.raw -f u8 -ar 8000
   ```

   Simpler check: the file should be **8000 bytes per second of audio**. A 12-second
   chant is ~96 000 bytes. If it is 5x that, the export did not resample.

2. **Polish it.** ffmpeg does not do the 200 Hz high-pass or the normalisation, and
   both matter on a 28 mm driver with no volume control:

   ```sh
   python tools/raw_resample.py chant.raw chant8k.raw --in-rate 8000 --limit 16
   ```

   Same command handles a file exported at the wrong rate — pass its real rate as
   `--in-rate` and it resamples properly instead of aliasing.

   `--limit 16` is what makes it loud enough to hear across a room; without it the
   chant peaks at full scale but averages ~14 dB down. Add `--soft-clip 1.5` for
   another ~2.7 dB if it is still too quiet. See the loudness note under **Format**
   for why this cannot be fixed in hardware.

3. **Point the loader at it.** In `platformio.ini`, under `[env:loader]`:

   ```ini
   custom_chant_file = chant8k.raw
   custom_chant_args = --rate 8000
   ```

   These two must agree with each other and with step 1. Nothing validates them.

4. **Load and play:**

   ```sh
   pio run -e loader -t upload
   pio run -e loader -t sendchant
   pio run -e player -t upload
   ```

   The loader firmware has to be on the chip for `sendchant`, and the player has to go
   back on afterwards. `sendchant` erases and rewrites from address 0, so the old
   chant does not need clearing first.

5. **Check the player's log** (`pio device monitor -e player -p COM7`). It prints the
   header it found and, after each play, the rate it measured:

   ```
   chant: fmt 0, 8000 Hz, 96000 bytes (~12 s)
   played 96000 of 96000 samples in 12000 ms = 8000 Hz (asked for 8000)
   ```

   If the byte count and duration match what you exported, it is right.

Limits worth knowing before picking a chant: `spiflash_read_header()` accepts rates
from **2000 to 24000 Hz** and a length up to **1 MB minus the 16-byte header**, which
is ~2 minutes at 8 kHz. Longer than that needs the ADPCM format, which is specified
above but not implemented.

## Flash layout

Simple and explicit, so the firmware never has to guess:

```
0x000000  16 bytes  header:  magic "CHNT" | format byte | sample rate (u32 LE) |
                             length in bytes (u32 LE) | reserved
0x000010  ...       audio data
```

Format byte: `0` = raw 8-bit unsigned PCM, `1` = 4-bit IMA ADPCM.

A header costs nothing and means the player firmware reads the length instead of
having it recompiled in. Keep the writer (`tools/`) and the reader (`src/spiflash.c`)
in agreement — if the header changes, change both in the same commit.

## Getting the bytes in — three routes

**A. CH341A USB programmer (simplest while the flash is on a DIP adapter).**
Write the chip on the desk with the adapter in the clip, then plug it into the
breadboard. Good for iterating on the audio without touching the MCU firmware.

**B. DediProg DP3000 at work.** March has access and has used MX25L series before.
This is the route for batch programming if the project ever becomes a small product —
worth confirming FM25Q08B is in the DediProg device list early.

**C. UART loader firmware (the tidy in-circuit route) — implemented.** A one-off
firmware in `src/loader/` receives the blob over PD5 TX / PD6 RX from any USB-serial
dongle and writes it to U3 page by page. Flash the loader, send the audio, then flash
the real player back:

```sh
make -f Makefile.loader flash
python tools/send_chant.py COM7 chant.raw
make flash
```

The header labels are MCU-side, so TX and RX cross: adapter RX → PD5, adapter TX → PD6,
GND → GND. A WCH-LinkE is both the debug probe and the USB-serial adapter; use its
serial pins for this. Do not power the board from the adapter and the CR123A at the
same time.

The wire protocol lives in [`../src/loader/README.md`](../src/loader/README.md). In
short: one ASCII byte per command, every reply starting with a status byte, fixed
256-byte pages with a checksum, and a CRC32 the MCU computes over the whole image at
the end. A 1 MB load takes about two minutes including verification.

The firmware does the usual dance internally — `06h` write enable before each page
program, `20h` sector erase before programming, `05h` BUSY polled after every write —
so none of that leaks into the protocol.

## Verification

Always verify before trusting a load — a silent bad byte in the middle of a chant is
hard to diagnose by ear. `send_chant.py` does it by default: the MCU computes a CRC32
over everything it wrote and sends four bytes back, and the host compares that against
`zlib.crc32` of the same image. Reading the whole megabyte back over the UART would
have cost as long again as writing it.

`--no-verify` skips it. There is no good reason to.
