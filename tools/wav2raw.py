#!/usr/bin/env python3
"""
wav2raw.py — chant recording -> flash blob for the chant clicker.  STUB.

Output layout (must stay in sync with src/spiflash.h and docs/audio-pipeline.md):

    0x000000  16 B  "CHNT" | format u8 | pad u8 | rate u32 LE | length u32 LE | pad
    0x000010  ...   audio data

Format byte: 0 = raw 8-bit unsigned PCM, 1 = 4-bit IMA ADPCM.

Intended use:

    ./wav2raw.py chant.wav chant.bin              # 8 kHz, 8-bit PCM
    ./wav2raw.py chant.wav chant.bin --adpcm      # 4-bit IMA ADPCM, ~2x longer
    ./wav2raw.py chant.wav chant.bin --peak -1    # normalise to -1 dBFS

Design notes worth not forgetting:
  * Keep samples at FULL SCALE. Volume is set in hardware by the R2/R3 divider.
    Attenuating here throws away resolution and adds hiss.
  * High-pass around 200 Hz first: the 28 mm driver starts near 700 Hz, so low
    content only eats headroom.
  * Print the CRC32 so the loader's reported CRC can be compared directly.
  * 1 MB holds ~2 min of 8 kHz/8-bit PCM, 4+ min as ADPCM. Warn if it will not fit.
"""

import argparse
import struct
import sys
import zlib

MAGIC = b"CHNT"
FMT_PCM8 = 0
FMT_ADPCM4 = 1
HDR_SIZE = 16
FLASH_SIZE = 1024 * 1024
TARGET_RATE = 8000


def build_header(fmt: int, rate: int, length: int) -> bytes:
    hdr = MAGIC + struct.pack("<BBII", fmt, 0, rate, length)
    return hdr.ljust(HDR_SIZE, b"\x00")


def load_wav(path: str):
    """TODO: read the WAV, downmix to mono, high-pass ~200 Hz, resample to 8 kHz,
    normalise to the requested peak, return a list/array of floats in [-1, 1].

    Dependency choice is open: `soundfile` + `scipy.signal.resample_poly` is the
    clean route; stdlib `wave` + `audioop` avoids dependencies but does a cruder
    resample. Pick one and record it here."""
    raise NotImplementedError("load_wav")


def to_pcm8(samples) -> bytes:
    """TODO: float [-1,1] -> unsigned 8-bit, i.e. round(s * 127.5 + 127.5),
    clipped to 0..255. Unsigned because the byte goes straight into the PWM
    compare register, where 128 is the idle mid-point."""
    raise NotImplementedError("to_pcm8")


def to_ima_adpcm(samples) -> bytes:
    """TODO: standard IMA ADPCM, 4 bits per sample, two samples per byte
    (low nibble first). The MCU-side decoder must use the same step/index
    tables — keep them in one place and copy, do not re-derive."""
    raise NotImplementedError("to_ima_adpcm")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input", help="source recording (wav)")
    ap.add_argument("output", help="flash blob to write")
    ap.add_argument("--adpcm", action="store_true", help="4-bit IMA ADPCM instead of PCM8")
    ap.add_argument("--peak", type=float, default=-1.0, help="normalise to this dBFS")
    args = ap.parse_args()

    samples = load_wav(args.input)
    if args.adpcm:
        data, fmt = to_ima_adpcm(samples), FMT_ADPCM4
    else:
        data, fmt = to_pcm8(samples), FMT_PCM8

    total = HDR_SIZE + len(data)
    if total > FLASH_SIZE:
        print(f"too big: {total} B > {FLASH_SIZE} B "
              f"({'try --adpcm' if not args.adpcm else 'trim the chant'})",
              file=sys.stderr)
        return 1

    blob = build_header(fmt, TARGET_RATE, len(data)) + data
    with open(args.output, "wb") as fh:
        fh.write(blob)

    secs = len(data) / TARGET_RATE * (2 if args.adpcm else 1)
    print(f"{args.output}: {total} B, ~{secs:.1f} s, crc32 {zlib.crc32(blob):#010x}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
