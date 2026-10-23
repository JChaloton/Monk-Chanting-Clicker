# Debug log — the bugs that shaped this firmware

Everything in here has already been fixed. It is written down because every one of these
presented as *the same symptom* — "no sound" or "the button does nothing" — and because
the fixes look arbitrary unless you know what they are defending against. If something
regresses, this is the list to read before re-deriving it all.

Chronological, oldest first. Commits: `d1e10bf` (first hardware run) and `2f75ba1` (sound).

---

## 0. Before any of this: nothing had ever compiled

The repo's first commit was design docs plus a firmware skeleton, written from
datasheets without a toolchain attached. The first real session found the boring
blockers first:

- **`funconfig.h` did not exist.** `ch32fun.h` includes it unconditionally and the
  framework ships no default. Every project must provide one.
- **Upstream renamed itself.** `ch32v003fun.h` → `ch32fun.h`, and the makefile fragment
  moved to `ch32fun/ch32fun.mk`. All sources and all three Makefiles had to follow.
- **`SysTicK_IRQn`** (capital K) is how some WCH headers spell it and is what the
  skeleton used; this checkout spells it `SysTick_IRQn`.

Lesson kept: none of the "written and syntax-checked" claims in the old docs meant
anything. The first bring-up session is where a project finds out whether it exists.

---

## 1. The host reconfigured the serial port mid-transaction

**Symptom.** The loader answered `C` (checksum mismatch) to page after page that the
host had built perfectly. Retries did not help; the CRC never got a chance to run.

**Cause.** `send_chant.py::_read()` used to set `self.ser.timeout` per call. On Windows
that setter *reconfigures the already-open port*, and a reconfigure in the middle of a
transaction can discard bytes that have been written but not yet clocked out. The
261-byte page in flight was truncated, so the loader's checksum legitimately failed on a
page that was correct when it left the host.

**Fix.** `_read()` is a deadline loop over a port whose settings never change: a short
fixed `timeout=0.05` at construction, and the real deadline layered on top in Python.
**Never reassign `ser.timeout` mid-transfer.**

This one blocked the entire transfer. It is the reason the audio load exists at all.

---

## 2. One dropped byte turned into a storm of spurious erases

**Symptom.** After a single corrupted byte, the loader started answering `K` to things
the host had not asked, the host's resync stopped working, and sectors were being erased
that nobody requested.

**Cause.** The loader's operand reads (`cmd_program`, `rx_addr24`, `rx_u32`) were
unbounded. A byte lost to noise parked the loader mid-command, waiting for a byte the
host had already finished sending — while the host waited for a status byte the loader
would never send. Both ends wedged. Then the host's `?` resync probes were consumed *as
operand bytes*, and three of them complete a 24-bit address: a real `S` sector erase,
answered with a genuine `K`. The host could not tell those replies from real ones.

**Fix.** Every operand read is bounded at **50 ms** (`uart_rx_to()`), and any short read
is reported as `C` or `E` — statuses the host already knows how to retry. The 50 ms is
chosen to sit in the gap between two timescales: far above the ~174 µs inter-byte gap of
a back-to-back burst at 57600 (it was ~87 µs at the 115200 the bound was written for),
so a healthy transfer never trips it and USB's 1 ms frame scheduling cannot either, and far below the host's 3 s status timeout. Because it
only fires after 50 ms of silence, the host has demonstrably stopped sending, so there
are no stray page bytes left in flight to be misread as commands.

Host side got the matching discipline: `_drain()` reads until the line has been *silent*
rather than flushing once (the USB bridge buffers, so a slow command can still be
delivering replies long after you gave up on it), and `sync()` sends one `?` per attempt
instead of a burst.

**Also:** `LOADER_BAUD` dropped 115200 → **57600**. `board.h` already warned that the HSI
is only ~1 % accurate; on hand-wiring, 115200 corrupted bytes faster than the resync
machinery could recover. 450 KB still loads in ~86 s.

---

## 3. The sample clock was SysTick, and SysTick is ch32fun's timebase

**Symptom.** One press, then hiss forever. The board never came back.

**Cause.** `audio.c` originally used SysTick for the 8 kHz tick, because 48 MHz / 8000 =
6000 divides exactly. But **ch32fun builds `Delay_Us()` and `Delay_Ms()` on the
free-running SysTick counter** (`DelaySysTick()` spins on `SysTick->CNT`). Reprogramming
SysTick for the sample rate broke every delay in the firmware, and stopping it with
`CTLR = 0` froze `CNT`, so the next `Delay_*()` never returned. The board hung inside
`audio_stop()`'s anti-pop ramp — with the amp still enabled. Hence: hiss forever.

**Fix.** **TIM2.** It divides the same 48 MHz clock just as exactly (`ATRLR = 5999`),
it is otherwise unused, and it leaves ch32fun's timebase alone.

**Bonus.** Freeing SysTick made it usable as an *independent* measurement clock, which is
why `play_range()` can now report the sample rate it actually achieved — a TIM2
misconfiguration cannot hide by skewing its own measurement.

**Postscript.** The LED demo that lived in `src/demo/` was never migrated and kept its
SysTick auto-reload tick, which made it good for exactly one play per reset — the same
bug, left standing in a side firmware. It has since been deleted rather than fixed; the
player is the thing that has to be right.

---

## 4. The chant was a 44.1 kHz file with 8000 stamped in its header

**Symptom.** Silence. Not slow audio — *silence*, or something you had to put your ear
against the speaker to notice. Meanwhile the firmware's own diagnostics insisted the
sample clock was a perfect 8000 Hz.

**Cause.** `NaKomNamoCompressed.raw` really was 44.1 kHz. `send_chant.py` writes whatever
`--rate` says into the header **without ever looking at the audio**, and the player takes
the header at its word. Nothing in the chain can detect the mismatch: the load succeeded,
the CRC verified, and the chant played 5.5× slow — which dropped the voice's ~400 Hz
fundamental to ~70 Hz, an octave and a half below the 28 mm driver's 700 Hz corner. So
the speaker simply could not reproduce it. The firmware was doing exactly as told.

**Diagnosis that worked.** Two diagnostics, both kept behind `PLAYER_SELFTEST`:

1. **A 1 kHz test tone straight out of TIM1** — no flash, no interrupt, no buffers. It
   halves the problem: audible tone ⇒ PWM, RC filter, divider, amp and speaker are all
   good, so the fault is upstream in the samples or the sample clock.
2. **A rate sweep** — replay the same flash bytes at 22050 / 32000 / 44100 / 48000 Hz,
   two seconds each, and listen. Reloading 450 KB over the UART to test one guess costs
   ~80 s; this costs two seconds a guess and needs no reload at all. 44100 sounded right,
   which named the bug immediately.

**Fix.** `tools/raw_resample.py` — resample the file so it genuinely *is* 8 kHz, with the
anti-alias low-pass that naive decimation skips, plus the 200 Hz high-pass and
normalisation `ffmpeg -ar` will not do:

```sh
python tools/raw_resample.py NaKomNamoCompressed.raw chant8k.raw --in-rate 44100
```

**The trap is still open, by design.** A headerless `.raw` carries no rate, so nothing
validates `--rate` against the file. Two ways to be sure: run `ffprobe` on the source
before converting and pass the same number to both `-ar` and `--rate`; or check the size
— **8000 bytes per second of audio**, so a 10-second chant is ~80 KB. If it is 5× that,
the export did not resample.

Do **not** "fix" a wrong-rate file by putting its native rate in the header instead. It
plays, but 44.1 kHz costs 5.5× the flash for detail the speaker cannot resolve — 1 MB
falls from ~2 minutes to ~23 seconds.

---

## 5. It played, and it was too quiet

**Symptom.** Correct pitch, correct duration, correct everything — and you had to lean in
to hear it.

**Cause, and why it is not a hardware problem.** Peak normalisation sets the *peaks* and
says nothing about the average. `NaKomNamo.raw` peaks at full scale but its RMS sits
13.7 dB below that, so the amp idles through almost the whole chant. And there is no
remaining fix in the schematic: R2/R3 already present ~0.32 Vpp against the PAM8302A's
~0.37 Vpp input ceiling, so the analog chain is flat out. **The wasted headroom is inside
the file.**

**Fix.** A look-ahead peak limiter in `raw_resample.py`. Peaks still land at
`--headroom`, so the amp sees exactly the voltage it saw before and never clips harder —
only the gap between peak and average closes.

| flags | crest factor | gain vs peak-normalise alone |
|---|---|---|
| *(none)* | 13.7 dB | — |
| `--limit 16` | 10.3 dB | **+3.4 dB** |
| `--limit 16 --soft-clip 1.5` | 7.5 dB | **+6.2 dB** |

**`--limit 16`, no soft clip, is what ships** — `chant8k-limit.raw`, confirmed by ear on
the hardware. `--soft-clip` bends the waveform rather than rescaling it, so it is held in
reserve.

**The look-ahead detail is worth remembering.** A limiter without look-ahead makes this
*worse*: the gain has not finished falling when the transient passes, so peaks survive at
full height while sustained material gets pulled down. Measured, that turned 13.7 dB into
14.9 dB — the exact opposite of the goal. Two milliseconds of look-ahead turns it into
10.3 dB.

**Two things that look like they should help and do not:**

- Raising `--highpass` toward the driver's 700 Hz corner *costs* loudness here
  (−14.1 → −15.0 dBFS at 400 Hz). The peaks in this recording are not LF-driven, so
  removing in-band energy loses more than the headroom it frees.
- Raising R3 to lift the divider ratio pushes the amp input past 0.37 Vpp into clipping.

Leave both alone.

---

## 6. The SWIO loader was built on a tool that cannot open this probe

Route D (`tools/load_audio_swio.py`) was written against `minichlink`, because that is
what the docs and the Makefiles use. On this machine it could never have run:

```
> minichlink -i
Error: couldn't open wch link device (libusb_open() = -5)
```

`-5` is `LIBUSB_ERROR_ACCESS`. The LinkE enumerates as `VID_1A86&PID_8010`
(`WCH-LinkRV`, RISC-V mode) bound to **WCH's own driver**. OpenOCD and `wlink` speak to
that driver and work; `minichlink` wants a WinUSB binding and cannot open the device at
all. This is the same wall that `make flash` hits, already recorded in `hardware.md` —
it was read as "prefer PlatformIO for flashing" when it actually means *nothing in this
tree may assume minichlink runs here*.

Fixed by giving `swio_link.py` an OpenOCD backend and defaulting to it. It is also much
faster: one OpenOCD process stays up for the whole load instead of a process launch per
chunk.

Lesson kept: a constraint recorded about one tool is a constraint about the machine. Check
which binary can actually open the probe before building on it — `minichlink -i` and
`openocd -f wch-riscv.cfg -c init` answer it in two seconds.

---

## 7. The chunk ack is about halted time, not wall-clock time

The loader does not acknowledge every chunk on the minichlink backend, and the reasoning
written down for it was: a 512-byte chunk is two page programs, under 6 ms, while the
next chunk cannot arrive for 250 ms — forty times the margin. True there. Carried over
to the OpenOCD backend as an optimisation, it corrupted the image:

```
error: CRC32 mismatch: MCU says 0x9f1058c5, host says 0xce244443
```

**What matters between two chunks is not elapsed time but how long the core is actually
running**, and nearly all of an OpenOCD chunk is spent halted. The sequence is halt →
write the scratch file → `load_image` → resume, and the loader only executes between
that resume and the next halt — a few milliseconds, against up to 6 ms of page
programming. So it was still mid-write when the next chunk overwrote its mailbox. The
minichlink backend is safe for the opposite reason: the core runs throughout the next
process's ~250 ms of startup and USB enumeration.

Fixed by making the per-chunk ack mandatory on any fast backend, with the reasoning in
`program_image()`'s docstring so it does not get optimised away again.

Lesson kept: with a debug probe, "how much time passed" and "how much time the target
got" are different numbers, and only the second one is a margin. The whole-image CRC32
is what turned a silent corruption into a one-line failure.

---

## The pattern worth keeping

Four different bugs, one symptom. What actually shortened each one:

1. **Halve the chain with a test that needs less of it.** The 1 kHz tone removes flash,
   interrupts and buffers from the picture in one move.
2. **Measure the thing you are assuming.** `play_range()` reports the sample rate it
   *achieved*, from an independent clock. That is what proved the firmware innocent and
   pointed at the file.
3. **Make the expensive experiment cheap.** The rate sweep replaced an 80-second reload
   per guess with two seconds per guess.
4. **Print enough at boot to rule out whole layers.** JEDEC ID, header fields, a real
   sample of the audio, the idle switch level. Every one of those lines exists because
   not having it cost time.
