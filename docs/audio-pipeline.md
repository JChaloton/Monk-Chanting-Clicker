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

`tools/wav2raw.py` (stub) does this conversion. See its header for the intended CLI.

## Flash layout

Simple and explicit, so the firmware never has to guess:

```
0x000000  16 bytes  header:  magic "CHNT" | format byte | sample rate (u32 LE) |
                             length in bytes (u32 LE) | reserved
0x000010  ...       audio data, page-aligned
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

**C. UART loader firmware (the tidy in-circuit route).** A one-off firmware in
`src/loader/` that receives the blob over PD5 TX / PD6 RX from any USB-serial dongle
and writes it to U3 page by page, then reports a checksum. Flash the loader, send the
audio, then flash the real player firmware.

Suggested minimal loader protocol (keep it dumb):

```
host → mcu : 'C' 'H' 'N' 'T'  <len u32 LE>       start
mcu  → host: 'K'                                 ack, flash erased
host → mcu : 256 bytes                           one page
mcu  → host: 'K'                                 page written and verified
...repeat...
mcu  → host: 'D' <crc32 u32 LE>                  done
```

Erase before program (`20h` per 4 KB sector, or `D8h` per 64 KB block), `06h` write
enable before each page program, and poll `05h` BUSY after every write.

## Verification

Always read back and compare before trusting a load — a silent bad byte in the middle
of a chant is hard to diagnose by ear. `tools/wav2raw.py` should print the CRC32 of the
blob it writes so the loader's reported CRC can be compared directly.
