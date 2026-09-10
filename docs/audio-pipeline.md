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

`tools/wav2raw.py` is still a stub. Until it exists, ffmpeg does the same conversion:

```sh
ffmpeg -i chant.wav -ac 1 -ar 8000 -f u8 -acodec pcm_u8 chant.raw
```

What ffmpeg will not do is the ~200 Hz high-pass and the peak normalisation, which is
the reason to finish `wav2raw.py` eventually. `tools/send_chant.py` writes the 16-byte
header itself, so a headerless `.raw` from ffmpeg is directly loadable.

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
