# LED wave demo — running the player on a bare dev board

The amp and flash breakouts are not built yet. The hard part of the firmware does not
depend on either of them: a sample clock that never jitters, feeding a PWM compare
register in real time. This demo runs exactly that path on a CH32V003 dev board and
shows the result as **LED brightness** instead of sound.

If the LED breathes with the chant, the timing core is correct, and stage 6 of
[`bring-up.md`](bring-up.md) becomes "connect the amp" rather than "write and debug the
player with an amplifier in the way".

| | |
|---|---|
| Firmware | [`src/demo/led_wave_demo.c`](../src/demo/led_wave_demo.c) |
| Tunables | [`src/demo/demo_board.h`](../src/demo/demo_board.h) |
| Converter | [`tools/wav2led.py`](../tools/wav2led.py) |
| Build | `make -f Makefile.leddemo` |

Nothing here touches PC3 (amp enable) or the SPI pins. U2 and U3 are not involved.

## Why the LED cannot just play the waveform

Audio swings symmetrically about the mid-point — 128, in our unsigned-8 format. Push
those bytes straight into the PWM duty at 8 kHz and the *average* duty is about 50 %
for every chant, every tone, and every not-quite-silence. Your eye is a low-pass filter
with roughly a 20 ms window, so it sees the average and nothing else: a constant half
brightness that tells you nothing.

So the demo rectifies and smooths first, and drives brightness from the **envelope** —
how loud — rather than the waveform. That is what a VU meter does, and it is what makes
a chant visible. `LED_GAMMA_SQUARE` in `demo_board.h` then squares the duty, because
the eye's response to light is roughly logarithmic; squaring cancels that out and makes
the LED look proportional to loudness rather than uniformly bright above quarter scale.

Turn the gamma off once and watch the difference. It is the clearest five seconds of
gamma-correction intuition you will get.

## What you need

- A CH32V003 dev board (nanoCH32V003, WCH CH32V003F4P6-EVT, or similar)
- A WCH-LinkE programmer
- One LED and one resistor — **330 Ω** is a good default
- Two jumper wires, or a breadboard
- Optionally: a push button, or just a wire you touch to GND

### The resistor

The rail is 3.3 V on most dev boards. A red LED drops about 2.0 V, so a 330 Ω resistor
gives (3.3 − 2.0) / 330 ≈ **4 mA** — bright enough to see clearly and comfortably under
the CH32V003's per-pin limit. Blue and white LEDs drop ~3.0 V and will be dim on a 3.3 V
rail; use a red, green, or yellow one. Do not fit an LED without a resistor.

## Step 1 — wire the LED

| From | To |
|---|---|
| **PD2** (U1 pin 19) | 330 Ω resistor |
| resistor, other end | LED **anode** (long leg, round side of the flat) |
| LED **cathode** (short leg, flat side) | **GND** |

PD2 is not an arbitrary choice: it is TIM1 CH1, the one pin the audio PWM can come out
of, and it is the pin the product schematic uses. Same timer, same channel, same pin,
same 187.5 kHz carrier — only what hangs off it differs. That is what makes the demo
evidence about the product rather than evidence about an LED.

If you would rather use the board's own LED, find its pin in your board's schematic and
set `PIN_LED_*` in `demo_board.h`. Most on-board LEDs are wired anode-to-rail and sink
through the pin, so also set `LED_ACTIVE_LOW 1` — otherwise the chant plays inside out:
bright in the silences, dark on the loud parts. You also give up TIM1 CH1, and with it
the part of the demo that mattered. Prefer PD2.

## Step 2 — the button (optional)

The firmware plays once at power-on, then replays on each press of **PD3** — the
product's own gesture, with the same 20 ms debounce.

| From | To |
|---|---|
| **PD3** (U1 pin 20) | one leg of a push button |
| other leg of the button | **GND** |

The internal pull-up is enabled in firmware, so no external resistor. No button? Touch a
jumper from PD3 to GND. Prefer hands-off? Set `DEMO_PRESS_TO_PLAY 0` in `demo_board.h`
and it loops on its own.

## Step 3 — connect the WCH-LinkE

| WCH-LinkE | Dev board |
|---|---|
| SWDIO / SWIO | **PD1** (U1 pin 18) |
| GND | GND |
| 3V3 | 3V3 — **only if the board is not powered over USB** |

Two rules, both learned the hard way by other people:

1. **Never feed 3V3 from the LinkE while the board is also powered over its own USB.**
   Pick one source. If the board has its own USB power, connect SWIO and GND only.
2. **Programmer and board must share GND** before you flash anything.

## Step 4 — toolchain

`ch32v003fun` is not vendored in this repo. Get it once:

```bash
git submodule add https://github.com/cnlohr/ch32v003fun ch32v003fun
```

You also need a RISC-V GCC and `make`. On Windows there are two workable routes:

- **Native:** install the xPack RISC-V embedded GCC and put it on `PATH`, and use Git
  Bash or MSYS2 for `make`. `minichlink` needs the WCH-LinkE bound to a WinUSB driver —
  use Zadig if `minichlink` reports that it cannot find the programmer.
- **WSL2:** `apt install gcc-riscv64-unknown-elf make`, then attach the LinkE into WSL
  with `usbipd-win`. Cleaner build, one extra step to plug the programmer in.

Either way, build `minichlink` once:

```bash
make -C ch32v003fun/minichlink
```

## Step 5 — convert the chant

```bash
python tools/wav2led.py chant.wav src/demo/chant_led.h
```

That is `--mode env`, the default: the envelope precomputed on the host at 100 Hz, about
100 bytes per second, so a whole 60 s chant costs 6 KB and fits in MCU flash alongside
the code. You see the entire chant.

Then do it again the other way:

```bash
python tools/wav2led.py chant.wav src/demo/chant_led.h --mode pcm8 --start 20 --duration 1.2
```

`--mode pcm8` is the genuine 8 kHz unsigned-8 stream — byte-for-byte what `wav2raw.py`
would put in the flash chip — and the MCU does the rectifying and smoothing itself,
inside the ISR, at the real sample rate. **This is the mode that actually rehearses the
player.** It costs length: 16 KB of MCU flash holds about 1.2 s, so use `--start` to
pick a representative second. The tool refuses to overflow and tells you what will fit.

Do both, in that order: `env` to confirm the wiring and enjoy the whole chant, then
`pcm8` to prove the timing core that `src/audio.c` will inherit. The two should look
alike on the LED — the converter deliberately uses the same envelope shape as the ISR.
If they look different, one of them is wrong, and that is worth chasing.

Source must be uncompressed PCM WAV. If yours is not:

```bash
ffmpeg -i chant.mp3 -ac 1 -ar 44100 -c:a pcm_s16le chant.wav
```

Skip this step entirely if you just want to check the wiring: with no `chant_led.h`
present, the firmware falls back to a built-in 1 Hz breathing ramp.

## Step 6 — build and flash

```bash
make -f Makefile.leddemo
make -f Makefile.leddemo flash
```

The demo builds separately from the player on purpose: it shares `board.h` and the pin
map, but none of the product's sources, so a stray edit here cannot end up in the thing
that ships. It compiles `led_wave_demo.c` alone — no `audio.c`, no `spiflash.c` — so it
builds while those are still skeletons.

## What you should see

At power-on the chant plays once through the LED, then again on every press of PD3.

- **`env` mode:** the LED breathes with the syllables of the whole chant.
- **`pcm8` mode:** the same, for a second or so, driven by real 8 kHz samples.

Two extra checks worth doing while it is on the bench:

- **Multimeter on PD2, DC range.** It should wander with the brightness. Hold the LED
  at full and you are near the rail; in the silences, near 0 V. This is the same
  measurement as stage 4 of `bring-up.md`, where a fixed 50 % duty must read ≈1.5 V.
- **Scope on PD2, if you have one.** You should see a 187.5 kHz square wave whose duty
  cycle moves. That frequency being exactly right is the whole point of the SysTick
  choice below.

## Troubleshooting

| Symptom | Cause |
|---|---|
| LED completely dark | Backwards. Cathode (short leg, flat side) goes to GND. Also check `LED_ACTIVE_LOW`. |
| LED at a constant half brightness | You are driving the raw waveform, not the envelope. Confirm the header's `CHANT_LED_MODE` matches the mode you generated. |
| Bright in the silences, dark on the loud parts | `LED_ACTIVE_LOW` is set the wrong way. |
| Faint flicker, no dynamics | Recording is quiet. Raise `LED_GAIN_Q4`, or re-run the converter with `--peak -1`. |
| Loud parts all look identical | Gain is clipping. Drop `LED_GAIN_Q4` back to 16. |
| Nothing at all, PD2 dead | TIM1 is an advanced-control timer — its outputs stay dead until `MOE` is set in `BDTR`. That line is in `pwm_init()`; check it survived any edit. |
| `SysTicK_IRQn` undeclared | Vendor header capitalisation. Some versions spell it `SysTick_IRQn`. Fix the one line in `tick_init()`. |
| `minichlink` cannot find the programmer | Windows driver. Bind the WCH-LinkE to WinUSB with Zadig, or attach it into WSL with `usbipd-win`. |
| Programmer stops detecting the chip | Firmware reconfigured PD1. Use the WCH tool's power-cycle unlock. Never touch PD1. |

## The decision this demo settles

`docs/decisions.md` lists an open question: the 8 kHz tick source. 187500 / 8000 =
23.4375, so no whole-number division of the TIM1 update event lands on 8 kHz.

The demo's answer is **SysTick**. 48 000 000 / 8000 = 6000 exactly. SysTick is otherwise
unused in this firmware, it is 32-bit with hardware auto-reload, and the ISR is one load
and one store. The carrier stays at exactly 187.5 kHz and the sample rate at exactly
8 kHz, so pitch error is the oscillator's rather than the divider's — instead of a
0.16 % flat chant from rounding the divisor.

Run `--mode pcm8`, confirm the timing on a scope, and then adopt the same tick in
`src/audio.c`. That is the demo paying for itself.

## What this does and does not prove

**Proves:** clocks and `SystemInit()`, PD2 as TIM1 CH1 alternate function, the advanced
timer's `MOE` gotcha, an 8-bit PWM at the real carrier frequency, an exact 8 kHz sample
tick under interrupt, sample data flowing byte-by-byte into a compare register in real
time, the PD3 pull-up and the debounce, and your whole build-and-flash toolchain.

**Does not prove:** anything about the SPI flash, the double buffer and its swap
discipline, the amp's shutdown sequencing, the anti-pop ramps, or the sleep current.
Those wait for the breakouts — but they are additions to a core you will already trust.
