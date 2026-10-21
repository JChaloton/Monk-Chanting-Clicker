# Bring-up

Never build the whole circuit and power it hoping for chants. Add one block, prove it,
then add the next — when something is wrong you then know it is in the block you just
added.

**Where this build actually is: stages 1–6 pass.** The perfboard build around a
CH32V003 dev board plays the chant on a press. Stage 7 (standby and the µA idle current)
is not implemented. The bugs found on the way through are in
[`debug-log.md`](debug-log.md); read that before re-debugging anything that regresses.

## Stages

**1 — Power only.** ✅ Battery, SW2, rail, C7. No chips. Multimeter: ~3.0–3.2 V between
rail and GND, 0 V with SW2 off.

**2 — Add U1 + J1.** ✅ Wire pins 9 (VDD), 7 (VSS), and the 3-pin header. Connect the
WCH-LinkE (3 V, SWIO, GND — pin-by-pin in
[`hardware.md`](hardware.md#the-wch-linke--pins-and-how-to-hook-it-up), and wire by the
dongle's printed labels, not by pin position) and confirm it detects the chip. Nothing
found at all: check the common ground first, then that the dongle is in RISC-V mode —
blue LED **off** when idle. Flash a program that toggles
PD2 once per second; watch the pin flip 0 ↔ 3 V. That one test proves power, soldering,
programming, and pin numbering at once.

**3 — Add SW1.** ✅ Enable the internal pull-up on PD3; toggle PD2 on each press. Verify
a press reads LOW. Multiple registrations per press = bounce; lengthen the debounce. The
player prints the idle level at boot (`switch idle level: HIGH/released`), which is a
free re-check of this stage every time you power up.

**4 — Add the audio chain up to C2 (no amp yet).** ✅ Output a fixed 50 % PWM. On the DC
range: **≈1.5 V** after R1 (the filter averaging), then **≈0.16 V** at the divider node.
Those two numbers are the whole analog front end verified.

**5 — Add U3.** ✅ Four SPI lines, WP# and HOLD# to the rail, C8 at pin 8. Send the JEDEC
ID command (`9Fh`) and print the reply: **`A1 40 14`**. Those three exact bytes prove all
four SPI wires in both directions in one shot.

Easiest way to see it: flash the player (`pio run -e player -t upload`) and run
`pio device monitor`. It prints
the ID at boot before anything else happens. `000000` means MISO (PC7) or the chip's 3 V
is wrong; `ffffff` means CS# (PC4) never gets pulled low. Do not debug anything else
until this one number is right. If it is intermittently wrong, **raise `FLASH_SPI_BR` in
`board.h` (slower clock) before suspecting the solder** — long unshielded SCK/DO runs on
perfboard are the usual cause.

**5b — Load the chant.** ✅ Only once stage 5 passes. One command:

```sh
python tools/load_audio.py --pio    # loader -> MCU, audio -> U3 (CRC-verified), player -> MCU
```

or by hand, which is the same three steps:

```sh
pio run -e loader -t upload                         # loader instead of the player
pio run -e loader -t sendchant                      # ends with a CRC match
pio run -e player -t upload                         # player back
```

(Make equivalents: `make load`, or `make -f Makefile.loader flash` /
`python tools/send_chant.py COM7 chant8k-limit.raw` / `make flash`.)

Wiring for the middle step and the wire protocol: [`../src/loader/README.md`](../src/loader/README.md).
The loader also reports the JEDEC ID on request, so it is a second opinion on stage 5.
This stage writes pages, erases sectors, and reads the whole chip back as a CRC — it is
the "write a page, read it back, compare" test at full scale.

**6 — Add U2 + speaker.** ✅ With PC3 LOW: **silence** — this is the PAM8302A pinout
sanity check, and it is free. Drive PC3 HIGH and play a test tone: a clean beep
(`PLAYER_SELFTEST 1` in `main.c` gives you 1 kHz for 500 ms at boot). Then, with the
chant already in U3 from stage 5b, `pio run -e player -t upload` and press the switch.

`pio device monitor` shows what the player thinks it has — format, rate, length, seconds — and
after each play, the sample rate it actually achieved:

```
chant: fmt 0, 8000 Hz, 81968 bytes (~10 s)
played 81968 of 81968 samples in 10246 ms = 7999 Hz (asked for 8000)
```

If it says *no valid CHNT header*, stage 5b did not take. If the achieved rate matches
but it sounds slow or silent, the **file** is not the rate its header claims — that is
the bug in [`debug-log.md`](debug-log.md) §4, and `PLAYER_SELFTEST`'s rate sweep names it
in about ten seconds.

**7 — Power discipline.** ⬜ **Not done.** Add standby/wake and measure idle current
(µA range, in series with the battery). Target ~9 µA. Today the amp and flash are already
in their < 1 µA states between plays, but the MCU polls, so bench idle is a few mA.

Before attempting it, know the escape hatch: standby changes how the debug interface
behaves, and if the WCH-LinkE stops connecting, `minichlink -u` power-cycle-unbricks the
chip. Never reconfigure PD1.

## Pre-power checklist — every time

- [ ] Pin-1 dots on U1 and U2 match the adapter's pin-1 corner. A reversed chip usually
      dies instantly.
- [ ] Continuity beep from each chip leg to its DIP pin; **no** beep between neighbouring
      pins (solder bridges).
- [ ] No beep between 3 V rail and GND with the battery out — a beep is a short.
- [ ] Speaker touches only VO+ (U2 pin 5) and VO− (U2 pin 8). **Never ground, never the
      rail.**
- [ ] C7's stripe (negative) faces GND.
- [ ] R4 (100 k SD pull-down) in place, so the amp stays silent until the MCU says
      otherwise.
- [ ] U3's WP# and HOLD# tied to the rail — a floating HOLD# causes maddening
      intermittent SPI failures.
- [ ] Programmer and board share GND before flashing.
- [ ] Not powering from the USB-serial adapter and the CR123A at the same time.

## Known-good expected values

| Test point | Expected |
|---|---|
| Rail, SW2 on | 3.0–3.2 V fresh cell |
| After R1, 50 % duty, DC range | ≈1.5 V |
| Divider node B, 50 % duty | ≈0.16 V |
| `9Fh` reply from U3 | `A1 40 14` |
| Amp with PC3 LOW | silent, < 1 µA |
| Player boot log, JEDEC | `flash JEDEC id a14014 (expect a14014)` |
| Player boot log, samples at 1 s | `min` and `max` clearly different (flat = no audio loaded) |
| Achieved rate after a play | within a few Hz of the header's rate |
| Whole board idle, sleeping | ~9 µA — **unverified, stage 7 not built** |

## If it goes wrong

| Symptom | Cause |
|---|---|
| Clicks then reboots mid-chant | Decoupling. C4/C5/C6 too far from their pins, or C7 missing. |
| Sound with PC3 LOW, or dead with PC3 HIGH | SD is on the wrong pin; your chip's pinout is not the one you wired. Recheck against the Diodes datasheet. |
| Intermittent SPI garbage | Floating HOLD# or WP#, a long clock wire, or `FLASH_SPI_BR` too fast. |
| JEDEC ID `00 00 00` or `FF FF FF` | MISO or CS# wrong, or the flash is not powered / still in deep power-down (send `ABh` first). |
| One press, then hiss forever | The sample clock took over SysTick and a delay never returned. It must be TIM2. See `debug-log.md` §3. |
| `played 0 of N samples` | The TIM2 interrupt never fired — check `NVIC_EnableIRQ`, `DMAINTENR`, and that `INTFR` is cleared after `UG`. |
| Plays, but slow / silent / an octave low | The file is not the rate its header claims. `debug-log.md` §4. |
| Plays correctly but far too quiet | Crest factor. Re-run `raw_resample.py --limit 16`. `debug-log.md` §5. |
| Loader answers `C` to every page | Host-side serial reconfiguration, or a bad link. `debug-log.md` §1–2. |
| Programmer stops detecting U1 | Firmware reconfigured PD1. `minichlink -u`. |
| Hiss on perfboard | Partly normal for class-D on hand-wiring. Shorten the speaker and decoupling leads; verify on a real PCB before chasing it. |
