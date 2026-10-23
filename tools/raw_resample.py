#!/usr/bin/env python3
"""
raw_resample.py — resample a headerless 8-bit unsigned PCM file to the player's
rate, with the anti-alias filter, high-pass and normalisation that ffmpeg's
plain `-ar` does not do.

Why this exists
---------------
`send_chant.py --rate N` writes N into the flash header without ever looking at
the audio, and the player takes the header at its word. So a .raw that is really
44.1 kHz loads happily, plays at 8 kHz, and comes out 5.5x slow and an octave
and a half low — quiet, too, because the whole voice lands below the speaker's
700 Hz corner. That was the "no sound / slow sound" bug during bring-up.

The fix is to make the file actually be 8 kHz. Dropping every 5.5th sample is
not enough: everything above 4 kHz in the source folds back down as aliasing
and turns into a metallic buzz. This does the windowed-sinc low pass first,
which is the part `ffmpeg -ar` gets right and naive decimation gets wrong.

Also applies the two steps docs/audio-pipeline.md asks for and ffmpeg will not:
a ~200 Hz high-pass (the speaker cannot reproduce below 700 Hz, so that energy
only eats headroom) and peak normalisation.

Making it louder
----------------
Peak normalisation alone leaves the chant quiet, and no change to the schematic
can fix that. R2/R3 already scale the PWM to ~0.32 Vpp against the PAM8302A's
~0.37 Vpp ceiling, so the analog chain is running flat out; the headroom that is
going to waste is *inside the file*. NaKomNamo.raw peaks at full scale but
averages 13.7 dB below it, so the amp spends almost all of its time idling.

`--limit` fixes that where it can actually be fixed. Peaks still land at
`--headroom`, so the amp sees exactly the voltage it does today and never clips
harder — only the gap between peak and average closes. Measured on
NaKomNamo.raw:

    --limit 16 --ratio 4                     13.7 -> 10.2 dB crest  (+3.5 dB)
    --limit 16 --ratio 4 --soft-clip 1.5     13.7 ->  7.5 dB crest  (+6.2 dB)

Reach for `--limit` first: it rescales the waveform and adds no distortion.
`--soft-clip` bends it, so it buys the last few dB at a price. Both default to
off, so an invocation without them behaves exactly as before.

Usage
-----
    python tools/raw_resample.py in.raw out.raw --in-rate 44100
    python tools/raw_resample.py in.raw out.raw --in-rate 44100 --out-rate 8000
    python tools/raw_resample.py in.raw out.raw --in-rate 8000 --limit 16

Then load it, telling the loader the rate it now really is:

    python tools/send_chant.py COM7 out.raw --rate 8000
"""

import argparse
import math
import sys

# Limiter timing. Not exposed on the command line: these are properties of
# speech, not of any particular chant, and the useful range is narrow. The
# look-ahead must be comfortably longer than the attack or the attack is
# pointless — see limiter().
LIMIT_ATTACK_MS = 1.0
LIMIT_RELEASE_MS = 60.0
LIMIT_LOOKAHEAD_MS = 2.0


def sinc(x: float) -> float:
    if x == 0.0:
        return 1.0
    px = math.pi * x
    return math.sin(px) / px


def design_lowpass(fc_norm: float, taps: int) -> list:
    """Windowed-sinc low pass. fc_norm is the cutoff as a fraction of the INPUT
    sample rate (so 0.5 is that file's Nyquist). Blackman window: the stopband
    is ~-74 dB, which is well under the 8-bit noise floor we are heading for,
    so the filter is not what limits quality here."""
    m = taps - 1
    h = []
    for i in range(taps):
        n = i - m / 2.0
        w = (0.42
             - 0.5 * math.cos(2.0 * math.pi * i / m)
             + 0.08 * math.cos(4.0 * math.pi * i / m))
        h.append(2.0 * fc_norm * sinc(2.0 * fc_norm * n) * w)
    s = sum(h)
    return [v / s for v in h]


def onepole_highpass(x: list, fc: float, rate: int) -> list:
    """First order high-pass. Gentle on purpose — this is for removing rumble
    and DC drift, not for shaping the sound."""
    dt = 1.0 / rate
    rc = 1.0 / (2.0 * math.pi * fc)
    a = rc / (rc + dt)
    y = [0.0] * len(x)
    prev_x = x[0]
    prev_y = 0.0
    for i in range(1, len(x)):
        prev_y = a * (prev_y + x[i] - prev_x)
        prev_x = x[i]
        y[i] = prev_y
    return y


def unit_scale(y: list) -> list:
    """Rescale so the largest magnitude is exactly 1.0."""
    p = max(abs(v) for v in y) or 1.0
    return [v / p for v in y]


def crest_db(y: list) -> float:
    """Peak-to-RMS ratio in dB — the number that decides how loud this plays.
    Peak normalisation cannot change it; the limiter exists to shrink it."""
    n = len(y) or 1
    peak = max(abs(v) for v in y)
    rms = math.sqrt(sum(v * v for v in y) / n)
    if peak <= 0.0 or rms <= 0.0:
        return 0.0                          # digital silence has no crest factor
    return 20.0 * math.log10(peak / rms)


def lookahead_peak(a: list, look: int) -> list:
    """For each sample, the largest magnitude in the next `look` samples. This is
    what lets the limiter start pulling the gain down *before* a transient
    arrives instead of after it."""
    n = len(a)
    return [max(a[i:i + look + 1]) for i in range(n)]


def limiter(y: list, thresh: float, ratio: float, rate: int) -> tuple:
    """Look-ahead peak limiter. Input and output are scaled to +/-1.

    This is the part that actually makes the chant louder. `raw_resample.py`
    used to do peak normalisation only, so a handful of transients defined the
    ceiling and the body of the chant sat ~14 dB below it — the amp's input
    divider was already at its limit while the average signal used a few per
    cent of the available power. Squashing the crest factor is the only way to
    get more loudness without asking the PAM8302A for more voltage than the
    3 V rail can give it. See docs/hardware.md.

    Why look-ahead: without it the attack is a race the transient always wins.
    The gain has not finished falling when the peak passes, so peaks survive at
    full height while sustained material gets pulled down — measured on
    NaKomNamo.raw that made the crest factor *worse* (13.7 -> 14.9 dB), which is
    the opposite of the point. Two milliseconds of look-ahead fixes it (-> 10.2 dB).
    The cost is 2 ms of latency, which for a file processed offline is free.

    Gain reduction is smoothed with a fast attack and a slow release, both in
    the gain domain rather than the envelope domain, so the release does not
    re-open on every glottal pulse."""
    n = len(y)
    if n == 0:
        return y, 1.0, 1.0                  # unity, not zero: main() logs 20*log10 of these

    look = max(1, int(LIMIT_LOOKAHEAD_MS * 1e-3 * rate))
    peak = lookahead_peak([abs(v) for v in y], look)

    c_att = math.exp(-1.0 / (LIMIT_ATTACK_MS * 1e-3 * rate))
    c_rel = math.exp(-1.0 / (LIMIT_RELEASE_MS * 1e-3 * rate))
    slope = 1.0 - 1.0 / ratio

    out = [0.0] * n
    g = 1.0
    g_min = 1.0
    g_sum = 0.0

    for i in range(n):
        p = peak[i]
        target = 1.0 if p <= thresh else (thresh / p) ** slope
        # Attack while pulling the gain down, release while letting it back up.
        c = c_att if target < g else c_rel
        g = c * g + (1.0 - c) * target
        out[i] = y[i] * g
        if g < g_min:
            g_min = g
        g_sum += g

    return out, g_min, g_sum / n


def soft_clip(y: list, drive: float) -> list:
    """tanh waveshaper, normalised so +/-1 maps to +/-1. Buys another 2-3 dB of
    RMS after the limiter has done what it can transparently, at the price of
    harmonic distortion — the limiter only rescales the waveform, this bends it.

    Keep `drive` low. On a chanting voice 1.0-1.5 is inaudible on a 28 mm
    driver, 2 gets edgy, 3+ is obviously crunchy. It is here to catch the
    residue the limiter leaves, not to do the work."""
    if drive <= 0.0:
        return y
    k = math.tanh(drive)
    return [math.tanh(drive * v) / k for v in y]


def resample(src: list, in_rate: int, out_rate: int, taps: int) -> list:
    """Polyphase-style windowed-sinc resample. Cut off at 45 % of the OUTPUT
    rate, expressed against the input rate — that is the whole point, and the
    step naive decimation skips."""
    fc_norm = 0.45 * out_rate / in_rate
    if fc_norm >= 0.5:
        fc_norm = 0.5                       # upsampling: no anti-alias needed
    h = design_lowpass(fc_norm, taps)
    half = taps // 2

    step = in_rate / out_rate
    n_out = int(len(src) / step)
    out = [0.0] * n_out
    n_src = len(src)

    for n in range(n_out):
        centre = int(n * step)
        acc = 0.0
        for k in range(taps):
            j = centre + k - half
            if 0 <= j < n_src:
                acc += src[j] * h[k]
        out[n] = acc
    return out


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("input", help="headerless 8-bit unsigned PCM")
    ap.add_argument("output", help="where to write the resampled .raw")
    ap.add_argument("--in-rate", type=int, required=True,
                    help="the rate the input file REALLY is")
    ap.add_argument("--out-rate", type=int, default=8000,
                    help="target rate (default 8000, matching board.h)")
    ap.add_argument("--highpass", type=float, default=200.0,
                    help="high-pass corner in Hz, 0 to skip (default 200)")
    ap.add_argument("--taps", type=int, default=97,
                    help="anti-alias filter length, odd (default 97)")
    ap.add_argument("--headroom", type=float, default=0.95,
                    help="peak normalisation target, 0..1 (default 0.95)")
    ap.add_argument("--limit", type=float, default=0.0, metavar="DB",
                    help="limiter threshold in dB below full scale, 0 to skip "
                         "(default 0 = off; 16 is a good starting point)")
    ap.add_argument("--ratio", type=float, default=4.0,
                    help="limiter ratio above the threshold (default 4)")
    ap.add_argument("--soft-clip", type=float, default=0.0, metavar="DRIVE",
                    help="tanh soft clip after the limiter, 0 to skip "
                         "(default 0 = off; 1.0-1.5 is safe, 2+ audible)")
    args = ap.parse_args()

    if args.ratio <= 1.0:
        print("--ratio must be greater than 1", file=sys.stderr)
        return 1

    raw = open(args.input, "rb").read()
    if not raw:
        print("input is empty", file=sys.stderr)
        return 1

    print(f"in : {len(raw)} bytes @ {args.in_rate} Hz = "
          f"{len(raw) / args.in_rate:.1f} s")

    # 8-bit unsigned centred on 128 -> signed float
    x = [b - 128.0 for b in raw]

    if args.highpass > 0.0:
        x = onepole_highpass(x, args.highpass, args.in_rate)

    y = resample(x, args.in_rate, args.out_rate, args.taps)

    crest_in = crest_db(y)
    peak_in = max(abs(v) for v in y) or 1.0

    # Dynamics run at the OUTPUT rate, on a +/-1 scale, between the resample and
    # the final peak normalisation: the limiter's attack and look-ahead are in
    # samples of the file the player will actually stream, and both stages need
    # a known full-scale reference to have thresholds mean anything. Rescaling
    # here also means the reported gain below stays comparable between runs.
    y = unit_scale(y)

    if args.limit > 0.0:
        thresh = 10.0 ** (-abs(args.limit) / 20.0)
        y, g_min, g_mean = limiter(y, thresh, args.ratio, args.out_rate)
        print(f"     limiter: -{args.limit:.0f} dBFS at {args.ratio:.0f}:1, "
              f"gain reduction {-20.0 * math.log10(g_min):.1f} dB max, "
              f"{-20.0 * math.log10(g_mean):.1f} dB mean")

    if args.soft_clip > 0.0:
        y = soft_clip(unit_scale(y), args.soft_clip)
        print(f"     soft clip: drive {args.soft_clip:.1f}")

    peak = max(abs(v) for v in y) or 1.0
    gain = (127.0 * args.headroom) / peak
    # What the gain would have been straight off the resampler, so the figure
    # means the same thing whether or not the limiter ran.
    gain_overall = (127.0 * args.headroom) / peak_in

    out = bytearray(len(y))
    clipped = 0
    for i, v in enumerate(y):
        s = int(round(v * gain)) + 128
        if s < 0:
            s, clipped = 0, clipped + 1
        elif s > 255:
            s, clipped = 255, clipped + 1
        out[i] = s

    open(args.output, "wb").write(bytes(out))

    print(f"out: {len(out)} bytes @ {args.out_rate} Hz = "
          f"{len(out) / args.out_rate:.1f} s")
    print(f"     gain x{gain_overall:.2f}, {clipped} samples clipped")
    print(f"     crest factor {crest_in:.1f} dB -> {crest_db(y):.1f} dB "
          f"({crest_in - crest_db(y):+.1f} dB of loudness, same peak voltage)")
    print()
    print(f"load it with:  python tools/send_chant.py COM7 {args.output} "
          f"--rate {args.out_rate}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
