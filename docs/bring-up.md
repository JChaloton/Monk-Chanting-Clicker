# Bring-up

Never build the whole circuit and power it hoping for chants. Add one block, prove it,
then add the next — when something is wrong you then know it is in the block you just
added. Breadboard, chips on their adapters.

## Stages

**1 — Power only.** Battery, SW2, rail, C7. No chips. Multimeter: ~3.0–3.2 V between
rail and GND, 0 V with SW2 off.

**2 — Add U1 + J1.** Wire pins 9 (VDD), 7 (VSS), and the 3-pin header. Connect the
WCH-LinkE (3 V, SWIO, GND) and confirm it detects the chip. Flash a program that
toggles PD2 once per second; watch the pin flip 0 ↔ 3 V. That one test proves power,
soldering, programming, and pin numbering at once.

**3 — Add SW1.** Enable the internal pull-up on PD3; toggle PD2 on each press. Verify a
press reads LOW. Multiple registrations per press = bounce; lengthen the debounce.

**4 — Add the audio chain up to C2 (no amp yet).** Output a fixed 50 % PWM. On the DC
range: **≈1.5 V** after R1 (the filter averaging), then **≈0.16 V** at the divider node.
Those two numbers are the whole analog front end verified.

**5 — Add U3.** Four SPI lines, WP# and HOLD# to the rail, C8 at pin 8. Send the JEDEC
ID command (`9Fh`) and print the reply: **`A1 40 14`**. Those three exact bytes prove
all four SPI wires in both directions in one shot.

Easiest way to see it: flash the player (`make flash`) and run `make monitor`. It
prints the ID at boot before anything else happens. `000000` means MISO (PC7) or the
chip's 3 V is wrong; `ffffff` means CS# (PC4) never gets pulled low. Do not debug
anything else until this one number is right.

**5b — Load the chant.** Only once stage 5 passes:

```sh
make -f Makefile.loader flash                  # loader instead of the player
python tools/send_chant.py COM7 chant.raw      # ~2 min, ends with a CRC match
make flash                                     # player back
```

Wiring for the middle step and the wire protocol: `../src/loader/README.md`. The
loader also prints the JEDEC ID on request, so it is a second opinion on stage 5 if
you want one. This stage writes a page, erases sectors, and reads the whole chip back
as a CRC — it is the "write a page, read it back, compare" test, at full scale.

**6 — Add U2 + speaker.** With PC3 LOW: **silence** — this is the PAM8302A pinout
sanity check. Drive PC3 HIGH and play a 440 Hz PWM tone: a clean beep. Then, with the
chant already in U3 from stage 5b, `make flash` and press the switch: the player reads
the header, wakes the flash, unmutes the amp, and streams the chant.

`make monitor` shows what the player thinks it has — format, rate, length, seconds. If
it says *no valid CHNT header*, stage 5b did not take.

**7 — Power discipline.** Add standby/wake and the flash power-down sequencing, then
measure idle current (µA range, in series with the battery). Target ~9 µA.

## Pre-power checklist — every time

- [ ] Pin-1 dots on U1 and U2 match the adapter's pin-1 corner. A reversed chip usually
      dies instantly.
- [ ] Continuity beep from each chip leg to its DIP pin; **no** beep between neighbouring
      pins (solder bridges).
- [ ] No beep between 3 V rail and GND with the battery out — a beep is a short.
- [ ] Speaker touches only VO+ (U2 pin 5) and VO− (U2 pin 8). **Never ground, never the
      rail.**
- [ ] C7's stripe (negative) faces GND.
- [ ] R4 in place, so the amp stays silent until the MCU says otherwise.
- [ ] U3's WP# and HOLD# tied to the rail — a floating HOLD# causes maddening
      intermittent SPI failures.
- [ ] Programmer and board share GND before flashing.

## Known-good expected values

| Test point | Expected |
|---|---|
| Rail, SW2 on | 3.0–3.2 V fresh cell |
| After R1, 50 % duty, DC range | ≈1.5 V |
| Divider node B, 50 % duty | ≈0.16 V |
| `9Fh` reply from U3 | `A1 40 14` |
| Amp with PC3 LOW | silent, < 1 µA |
| Whole board idle, sleeping | ~9 µA |

## If it goes wrong

- **Clicks then reboots mid-chant** → decoupling. C4/C5/C6 too far from their pins, or
  C7 missing.
- **Sound with PC3 LOW, or dead with PC3 HIGH** → SD is on the wrong pin; your chip's
  pinout is not the one you wired. Recheck against the Diodes datasheet.
- **Intermittent SPI garbage** → floating HOLD# or WP#, or a long breadboard clock wire.
- **JEDEC ID reads `00 00 00` or `FF FF FF`** → MISO or CS# wrong, or the flash is not
  powered / still in deep power-down (send `ABh` first).
- **Programmer stops detecting U1** → firmware reconfigured PD1. Use the WCH tool's
  power-cycle unlock.
- **Hiss on the breadboard** → normal for class-D on a breadboard. Shorten the speaker
  and decoupling leads; verify on the real PCB before chasing it.
