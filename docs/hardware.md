# Hardware reference

Everything electrical, with the reasoning. Source of truth for `src/board.h` and for
the KiCad schematic when it gets drawn.

Confirmed part list dates from 2026-08-21. Full illustrated walkthrough (block diagram
+ drawn schematic) lives in the "Chant Clicker Schematic" artifact:
https://claude.ai/code/artifact/a1a5eccc-82fa-48f8-8542-af769fb2e9c6

## Blocks

CR123A → SW2 (master power) → 3 V rail → { U1 MCU, U2 amp, U3 flash }.
SW1 signals U1. U1 streams audio out of U3 over SPI, emits it as PWM, and gates U2
with an enable line. U2 drives LS1 bridge-tied.

The switch is an *input to the MCU*, not a power or audio switch. That single decision
buys debounce, full-chant playback from a short press, amp shutdown between plays, and
µA-level sleep.

## Power

- CR123A: 3.0 V nominal (~3.2 V fresh, sagging toward 2.5 V near empty), ~1500 mAh.
- U1 needs 2.7–5.5 V, U2 needs 2.0–5.5 V, U3 needs 2.3–3.6 V. All three live on one
  unregulated 3 V rail. Consequence: the MCU's 2.7 V floor is what ends the battery's
  usable life, slightly before the cell is truly flat. Acceptable for a toy.
- Peak draw: class-D at ~0.5 W into 8 Ω at ~85 % efficiency ≈ 200 mA at the loudest
  peaks. A CR123A supplies ~1 A continuously, so there is generous margin.
- Idle target ≈ 9 µA (MCU standby + amp SD low + flash deep power-down) → years on one
  cell.

Decoupling is not optional. Class-D amps draw current in fast pulses; without local
reservoirs the rail dips and the MCU browns out mid-chant ("it clicks then reboots").

- C4 100 nF ceramic **at U1 pin 9**
- C5 100 nF ∥ C6 1 µF ceramic **at U2 pin 6**
- C8 100 nF ceramic **at U3 pin 8**
- C7 220 µF electrolytic (100–470 µF, ≥ 6.3 V) bulk anywhere on the rail — stripe to GND
- SW2 slide switch in series with battery + as a master off

"At the pin" means the shortest wire you can manage.

## U1 — CH32V003F4P6 (TSSOP-20)

RISC-V, 16 KB flash, 2 KB RAM, up to 48 MHz from an **internal** oscillator — so no
crystal, and PA1/PA2 stay free.

| Pin | Name | Use | Why this pin |
|---|---|---|---|
| 1 | PD4 | — | spare |
| 2 | PD5 | UART TX (debug / loader) | printf-style debugging, audio loader |
| 3 | PD6 | UART RX (loader) | spare otherwise |
| 4 | PD7 | NRST + R5 10k pull-up (optional) | keeps reset firmly high |
| 5 | PA1 | — | free, internal osc used |
| 6 | PA2 | — | free |
| 7 | VSS | GND | |
| 8 | PD0 | — | spare |
| 9 | VDD | 3 V + C4 | |
| 10–12 | PC0/PC1/PC2 | — | spare (PC1 SDA, PC2 SCL if ever needed) |
| 13 | PC3 | → U2 SD (amp enable) | plain GPIO; keeps timer pins free |
| 14 | PC4 | → U3 CS# | plain GPIO, software-driven |
| 15 | PC5 | → U3 CLK | **hardware SPI SCK lives here** |
| 16 | PC6 | → U3 DI | **hardware SPI MOSI** |
| 17 | PC7 | ← U3 DO | **hardware SPI MISO** |
| 18 | PD1 | SWIO → J1 | single-wire debug — keep dedicated |
| 19 | PD2 | audio PWM → R1 | **TIM1 CH1 — only timer-capable pins can do hardware PWM** |
| 20 | PD3 | ← SW1 (other side to GND) | interrupt-capable, wakes from standby |

Only PD2 (TIM1 CH1) and PC5/PC6/PC7 (SPI) are hard constraints. PC3, PC4, PD3 could
move to any free GPIO.

**J1 programming header:** 3 pins — 3 V, SWIO (PD1), GND. Two rules learned the hard
way: never reconfigure PD1 as ordinary GPIO in firmware (you can lock the programmer
out and need the WCH tool's power-cycle unlock), and always share ground with the
programmer.

## SW1 — the switch

Active-low: one side to PD3, other side to GND, internal pull-up (~30–50 kΩ) enabled
in firmware. No external resistor needed. A floating input reads garbage; a switch to
ground can never pull the pin above the supply.

Blue keyboard switches sometimes have extra legs / a diode slot for matrix wiring —
ignore those, only the two contacts matter.

Bounce: contacts chatter for a few ms. Handled in firmware (~20 ms lockout). An
optional 100 nF across the contacts helps but is not required.

## Audio chain — PD2 to the amp input

Three stages, in order. The MCU has no DAC; it can only drive PD2 fully high or low, so
audio is the *average* of a 187.5 kHz PWM whose duty is updated 8000×/s.

1. **Low-pass R1 1 kΩ + C1 15 nF.** f = 1/(2πRC) ≈ **10.6 kHz** — voice band passes,
   the 187.5 kHz carrier is knocked down >20×. Its real job is keeping strong
   ultrasonics from intermodulating in the amp's input stage.
2. **Divider R2 10 kΩ / R3 1.2 kΩ.** The PAM8302A's gain is fixed at ~×16 (24 dB). Its
   bridged output can swing ~6 Vpp, so the largest usable input is 6/16 ≈ 0.37 Vpp.
   The divider scales 3 Vpp by R3/(R2+R3) ≈ 0.107 → **≈ 0.32 Vpp**, just under
   clipping at full software volume. Attenuate in hardware, keep digital samples at
   full scale — scaling samples in software throws away resolution and adds hiss.
3. **Coupling C2 220 nF.** PWM output idles near 1.5 V DC; the amp holds its own input
   bias. Direct coupling wastes power and unbalances the amp. With the amp's ~10 kΩ
   input impedance, 220 nF passes everything above ~70 Hz.

**C3 220 nF from IN− to GND** balances the differential input so noise common to both
inputs cancels. Keep C2 and C3 equal.

## U2 — PAM8302A (MSOP-8)

⚠ Official Diodes pinout — several tutorial sites (components101, watelectronics)
print a different, wrong order:

| Pin | Name | Use |
|---|---|---|
| 1 | NC | leave empty |
| 2 | IN+ | ← C2 |
| 3 | IN− | C3 220 nF to GND |
| 4 | SD | ← U1 PC3, plus R4 100 kΩ to GND |
| 5 | VO+ | speaker terminal A |
| 6 | VDD | 3 V + C5/C6 |
| 7 | GND | GND |
| 8 | VO− | speaker terminal B |

**Class-D, filterless:** outputs are ~300 kHz switching waveforms; the speaker's own
inductance smooths them. So no output filter parts — but keep speaker wires short (a
few cm) and ideally twisted; a long loop is an RF antenna.

**Bridge-tied load: neither speaker terminal may ever touch ground.** Grounding VO−
short-circuits half the amp. Speaker goes to pins 5 and 8 and nothing else. Swapping
the two terminals is harmless (just inverts the waveform).

**SD is active-low shutdown:** LOW → < 1 µA, HIGH → awake in ms. R4 100 kΩ to GND
matters: while U1 boots or is being reprogrammed its pins float, and a floating SD
could wake the amp and hiss. The pull-down makes "off" the default.

*Free sanity check:* once powered, the amp must be **silent** with PC3 LOW. Sound with
PC3 LOW, or silence with PC3 HIGH, means the SD wire is on the wrong pin — evidence
about which pinout your chip really has, before anything is damaged.

## LS1 — HDK-2808BB-7

28 mm, 8 Ω, 1.5 W, ~700 Hz–5 kHz, 97 dB. At 3 V into 8 Ω the amp tops out near 0.5 W
(the datasheet's 0.6 W is at 3.6 V) — a third of the driver's rating, so nothing burns
out at full volume. The narrow band suits a chanting voice (intelligibility lives
~300 Hz–4 kHz). Seal the back against the enclosure and vent the front through a hole
pattern; a loose driver sounds thin.

## U3 — FM25Q08B (SOP-8, 1.27 mm)

8 Mbit = 1 MB. 2.3–3.6 V (note: its floor is *below* the MCU's, so the memory can never
be the first thing to fail as the cell fades). 50 MHz read. JEDEC ID `A1 40 14`.

| Pin | Name | Use |
|---|---|---|
| 1 | CS# | ← U1 PC4 |
| 2 | DO | → U1 PC7 / MISO |
| 3 | WP# | tie to 3 V rail |
| 4 | GND | GND |
| 5 | DI | ← U1 PC6 / MOSI |
| 6 | CLK | ← U1 PC5 / SCK |
| 7 | HOLD# | tie to 3 V rail |
| 8 | VCC | 3 V + C8 100 nF |

**Never leave an unused input floating.** WP# and HOLD# tied high are permanently
inactive; left floating they randomly write-protect or pause the chip in ways that are
miserable to debug. (Tying them high is also what quad-SPI would want, so this wiring
never has to change.)

Streaming needs only 64 kbit/s against a 50 MHz-capable bus — enormous slack. The bus
is idle almost all the time.

**Deep power-down (`B9h`) before MCU standby, wake with `ABh` on press.** Idle flash
current would otherwise dominate the sleep budget.

## Passives BOM

| Ref | Value | Job | Acceptable range |
|---|---|---|---|
| R1 | 1 kΩ | PWM low-pass with C1 | 470 Ω – 2.2 kΩ (rescale C1 to keep f ≈ 10 kHz) |
| R2 | 10 kΩ | divider, top | keep R2:R3 near 9:1 |
| R3 | 1.2 kΩ | divider, bottom | 1 – 1.5 kΩ |
| R4 | 100 kΩ | SD pull-down (amp off by default) | 47 – 220 kΩ |
| R5 | 10 kΩ | NRST pull-up (optional) | 4.7 – 47 kΩ, or omit |
| C1 | 15 nF | PWM low-pass with R1 | 10 – 22 nF |
| C2, C3 | 220 nF | input coupling / balance pair | 100 nF – 1 µF, keep both equal |
| C4, C5 | 100 nF | decoupling at U1 pin 9, U2 pin 6 | ceramic, at the pin |
| C6 | 1 µF | extra decoupling at U2 pin 6 | 1 – 10 µF ceramic |
| C8 | 100 nF | decoupling at U3 pin 8 | ceramic, at the pin |
| C7 | 220 µF | bulk reservoir across the rail | 100 – 470 µF, ≥ 6.3 V, electrolytic |
| SW2 | — | master power slide switch | — |

## Prototyping notes

- U1 and U2 are **0.65 mm pitch** (TSSOP-20 / MSOP-8) — use the fine-pitch side of a
  double-sided adapter board. The plain "SOP8/SOP20" side is 1.27 mm SOIC and the chip
  physically will not fit. U3 *is* standard 1.27 mm SOP-8 — plain side.
- Pin 1: hold the chip with the text readable, look for the dot/dimple (usually
  bottom-left); numbering runs counter-clockwise.
- After soldering to an adapter, beep continuity from each chip leg to its DIP pin, and
  between neighbouring pins (a beep there is a solder bridge).
- Breadboards add loose contacts and long loops, which class-D amps dislike. Some hiss
  or weaker output on the breadboard versus the final PCB is normal. Route grounds back
  toward the battery rather than daisy-chaining.

## Still to acquire

- WCH-LinkE programmer — the one tool the build cannot proceed without
- Optionally a CH341A USB programmer or USB-serial dongle for loading audio into U3
  (see `docs/audio-pipeline.md`); March also has DediProg DP3000 access at work for
  batch-programming SPI flash
