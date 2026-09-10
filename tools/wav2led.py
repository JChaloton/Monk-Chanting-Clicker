#!/usr/bin/env python3
"""
wav2led.py - chant recording -> C header for the dev-board LED demo.

This is the sibling of wav2raw.py. Same front end (mono, high-pass, resample,
normalise), different destination: instead of a 1 MB blob for the SPI flash, it
emits a small const array that fits in the CH32V003's own 16 KB of flash, so the
LED demo needs no flash chip and no programmer for U3.

    ./wav2led.py chant.wav src/demo/chant_led.h                  # whole chant
    ./wav2led.py chant.wav src/demo/chant_led.h --mode pcm8      # real 8 kHz path
    ./wav2led.py chant.wav src/demo/chant_led.h --start 12 --duration 8

Two modes, and the choice is a real trade-off:

  env  (default)  The envelope - loudness only - precomputed here at 100 Hz.
                  100 bytes per second, so a full 60 s chant costs 6 KB and fits
                  comfortably. You see the whole chant. What you are NOT testing
                  is the 8 kHz sample clock, because there isn't one.

  pcm8            The genuine 8 kHz unsigned-8 stream, byte-identical to what
                  wav2raw.py would put in the flash chip. The MCU does the
                  rectify/smooth itself, in the ISR, at the real sample rate.
                  This is the mode that actually rehearses the player. The cost
                  is length: MCU flash holds about 1.2 s of it, so pick a
                  representative second with --start.

Do both, in that order: `env` to confirm the wiring and enjoy the whole chant,
then `pcm8` to prove the timing core that src/audio.c will inherit.

Pure standard library on purpose - no numpy, no soundfile, works on the Python
that is already on the machine. It only reads uncompressed PCM WAV; if your
source is mp3 or compressed wav, convert it first:

    ffmpeg -i chant.mp3 -ac 1 -ar 44100 -c:a pcm_s16le chant.wav
"""

import argparse
import array
import math
import os
import struct
import sys
import wave

TARGET_RATE = 8000          # matches AUDIO_SAMPLE_RATE_HZ in src/board.h
MODE_PCM8 = "pcm8"
MODE_ENV = "env"

# 16 KB of MCU flash, minus room for the demo firmware itself. The demo builds
# to roughly 2 KB, so ~10 KB of audio leaves a healthy margin. Override with
# --budget if you have measured your own build.
DEFAULT_BUDGET = 10 * 1024


# --------------------------------------------------------------------------- #
# WAV front end
# --------------------------------------------------------------------------- #

def read_wav(path):
    """Return (samples as list of float in [-1, 1], sample rate), downmixed to mono."""
    with wave.open(path, "rb") as wf:
        nch = wf.getnchannels()
        width = wf.getsampwidth()
        rate = wf.getframerate()
        raw = wf.readframes(wf.getnframes())

    if width == 1:
        # WAV 8-bit is unsigned, mid-point 128.
        vals = [(b - 128) / 128.0 for b in raw]
    elif width == 2:
        a = array.array("h")
        a.frombytes(raw[: len(raw) // 2 * 2])
        if sys.byteorder == "big":
            a.byteswap()
        vals = [v / 32768.0 for v in a]
    elif width == 3:
        vals = []
        for i in range(0, len(raw) - 2, 3):
            v = raw[i] | (raw[i + 1] << 8) | (raw[i + 2] << 16)
            if v & 0x800000:
                v -= 0x1000000
            vals.append(v / 8388608.0)
    elif width == 4:
        a = array.array("i")
        a.frombytes(raw[: len(raw) // 4 * 4])
        if sys.byteorder == "big":
            a.byteswap()
        vals = [v / 2147483648.0 for v in a]
    else:
        raise SystemExit(f"unsupported sample width: {width * 8} bit")

    if nch > 1:
        mono = [sum(vals[i:i + nch]) / nch for i in range(0, len(vals) - nch + 1, nch)]
    else:
        mono = vals

    return mono, rate


def highpass(samples, rate, cutoff_hz):
    """One-pole high-pass. The 28 mm driver starts near 700 Hz, so anything below
    a couple of hundred Hz is headroom the chant never gets to use. Removing it
    here also stops DC offset and room rumble from holding the LED half-lit."""
    if cutoff_hz <= 0:
        return samples
    rc = 1.0 / (2.0 * math.pi * cutoff_hz)
    dt = 1.0 / rate
    a = rc / (rc + dt)
    out = [0.0] * len(samples)
    prev_x = samples[0] if samples else 0.0
    prev_y = 0.0
    for i, x in enumerate(samples):
        prev_y = a * (prev_y + x - prev_x)
        prev_x = x
        out[i] = prev_y
    return out


def resample(samples, src_rate, dst_rate):
    """Linear interpolation. Crude next to a polyphase resampler, and entirely
    good enough for 8 kHz speech-band audio driving an LED."""
    if src_rate == dst_rate:
        return samples
    n_out = int(len(samples) * dst_rate / src_rate)
    out = [0.0] * n_out
    step = src_rate / dst_rate
    for i in range(n_out):
        pos = i * step
        j = int(pos)
        frac = pos - j
        s0 = samples[j]
        s1 = samples[j + 1] if j + 1 < len(samples) else s0
        out[i] = s0 + (s1 - s0) * frac
    return out


def normalise(samples, peak_dbfs):
    """Scale so the loudest sample sits at peak_dbfs. Keep the audio at full
    scale: volume is set in hardware by the R2/R3 divider, and attenuating here
    throws away resolution."""
    peak = max((abs(s) for s in samples), default=0.0)
    if peak < 1e-9:
        return samples
    target = 10.0 ** (peak_dbfs / 20.0)
    g = target / peak
    return [s * g for s in samples]


# --------------------------------------------------------------------------- #
# Encoders
# --------------------------------------------------------------------------- #

def to_pcm8(samples):
    """float [-1,1] -> unsigned 8-bit. Unsigned because the byte goes straight
    into the PWM compare register, where 128 is the idle mid-point."""
    out = bytearray(len(samples))
    for i, s in enumerate(samples):
        v = int(round(s * 127.5 + 127.5))
        out[i] = 0 if v < 0 else (255 if v > 255 else v)
    return bytes(out)


def to_envelope(samples, rate, env_rate, release_ms):
    """Rectify, follow with instant attack and a one-pole release, then take the
    peak of each output frame. This is deliberately the same shape as the ISR in
    src/demo/led_wave_demo.c - the two modes should look alike on the LED, and
    if they don't, one of them is wrong."""
    tau = max(release_ms, 1.0) / 1000.0
    decay = math.exp(-1.0 / (tau * rate))

    step = rate / env_rate
    n_out = int(len(samples) / step)
    out = bytearray(n_out)

    env = 0.0
    idx = 0
    for i in range(n_out):
        end = int((i + 1) * step)
        frame_peak = 0.0
        while idx < end and idx < len(samples):
            mag = abs(samples[idx])
            env = mag if mag > env else env * decay
            if env > frame_peak:
                frame_peak = env
            idx += 1
        v = int(round(frame_peak * 255.0))
        out[i] = 0 if v < 0 else (255 if v > 255 else v)
    return bytes(out)


# --------------------------------------------------------------------------- #
# Output
# --------------------------------------------------------------------------- #

HEADER_TEMPLATE = """/*
 * chant_led.h - GENERATED by tools/wav2led.py. Do not edit; regenerate.
 *
 *   source   : {src}
 *   mode     : {mode_name}
 *   rate     : {rate} Hz
 *   length   : {length} bytes ({secs:.2f} s)
 *   command  : {cmd}
 */

#ifndef CHANT_LED_H
#define CHANT_LED_H

#include <stdint.h>

#define CHANT_LED_MODE  {mode_val}   /* 0 = MODE_PCM8, 1 = MODE_ENV */
#define CHANT_LED_RATE  {rate}u
#define CHANT_LED_LEN   {length}u

/* const, so the linker keeps this in flash and not in the 2 KB of RAM. */
static const uint8_t chant_led_data[CHANT_LED_LEN] = {{
{body}}};

#endif /* CHANT_LED_H */
"""


def emit_header(path, data, mode, rate, src, cmd):
    lines = []
    for i in range(0, len(data), 16):
        chunk = ", ".join("%3d" % b for b in data[i:i + 16])
        lines.append("    " + chunk + ",\n")

    text = HEADER_TEMPLATE.format(
        src=os.path.basename(src),
        mode_name=mode,
        mode_val=0 if mode == MODE_PCM8 else 1,
        rate=rate,
        length=len(data),
        secs=len(data) / rate,
        cmd=cmd,
        body="".join(lines),
    )
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text)


# --------------------------------------------------------------------------- #

def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input", help="source recording (uncompressed PCM wav)")
    ap.add_argument("output", help="C header to write, e.g. src/demo/chant_led.h")
    ap.add_argument("--mode", choices=[MODE_ENV, MODE_PCM8], default=MODE_ENV,
                    help="env = whole chant, loudness only (default); "
                         "pcm8 = real 8 kHz stream, ~1.2 s")
    ap.add_argument("--env-rate", type=int, default=100,
                    help="envelope sample rate in Hz, env mode (default 100)")
    ap.add_argument("--release", type=float, default=32.0,
                    help="envelope release time in ms (default 32, matching "
                         "LED_RELEASE_SHIFT 8 at 8 kHz)")
    ap.add_argument("--start", type=float, default=0.0, help="skip this many seconds")
    ap.add_argument("--duration", type=float, default=None, help="use this many seconds")
    ap.add_argument("--peak", type=float, default=-1.0, help="normalise to this dBFS")
    ap.add_argument("--highpass", type=float, default=200.0,
                    help="high-pass cutoff in Hz, 0 to disable (default 200)")
    ap.add_argument("--budget", type=int, default=DEFAULT_BUDGET,
                    help=f"fail if the array exceeds this many bytes "
                         f"(default {DEFAULT_BUDGET})")
    args = ap.parse_args()

    samples, rate = read_wav(args.input)
    if not samples:
        raise SystemExit("empty input")

    if args.start or args.duration is not None:
        a = int(args.start * rate)
        b = len(samples) if args.duration is None else a + int(args.duration * rate)
        samples = samples[a:b]
        if not samples:
            raise SystemExit("--start/--duration selected nothing")

    samples = highpass(samples, rate, args.highpass)
    samples = resample(samples, rate, TARGET_RATE)
    samples = normalise(samples, args.peak)

    if args.mode == MODE_PCM8:
        data, out_rate = to_pcm8(samples), TARGET_RATE
    else:
        data = to_envelope(samples, TARGET_RATE, args.env_rate, args.release)
        out_rate = args.env_rate

    if len(data) > args.budget:
        fits = args.budget / out_rate
        print(f"{len(data)} B exceeds the {args.budget} B budget "
              f"(~{fits:.2f} s at {out_rate} Hz).", file=sys.stderr)
        if args.mode == MODE_PCM8:
            print(f"  Trim it:  --start <s> --duration {fits:.2f}", file=sys.stderr)
            print("  Or see the whole chant with the default --mode env.",
                  file=sys.stderr)
        else:
            print(f"  Trim it, or lower --env-rate "
                  f"(50 Hz still looks fine and halves this).", file=sys.stderr)
        return 1

    cmd = " ".join([os.path.basename(sys.argv[0])] + sys.argv[1:])
    emit_header(args.output, data, args.mode, out_rate, args.input, cmd)

    secs = len(data) / out_rate
    print(f"{args.output}: {args.mode}, {out_rate} Hz, {len(data)} B, ~{secs:.2f} s "
          f"({100 * len(data) / args.budget:.0f} % of the {args.budget} B budget)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
