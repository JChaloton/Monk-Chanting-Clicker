# Hardware reference

Everything electrical, with the reasoning. Source of truth for `src/board.h` and for
the KiCad schematic when it gets drawn. **This file and `src/board.h` must change in the
same commit**, and where any other document disagrees about a reference designator or a
value, this one wins.

Confirmed part list dates from 2026-08-21. Full illustrated walkthrough (block diagram
+ drawn schematic) lives in the "Chant Clicker Schematic" artifact:
https://claude.ai/code/artifact/a1a5eccc-82fa-48f8-8542-af769fb2e9c6

**As built, 2026-09-11:** everything below is soldered onto perfboard around a CH32V003
dev board and works — bring-up stages 1–6 pass and the clicker plays the chant. The only
unverified number in this document is the ~9 µA idle figure, which depends on MCU standby
that is not implemented yet. See [`bring-up.md`](bring-up.md).

**Changed 2026-10-23 — the board is now a three-wire board.** PD5/PD6 carry no UART on
the current revision, and nothing needs them to: the chant loads over the SWIO wire that
already programs the chip. One connector, three wires, whole pipeline. Everything in
this file that used to require a serial adapter is marked as the perfboard's history
rather than the board's requirement. See [The WCH-LinkE](#the-wch-linke--pins-and-how-to-hook-it-up).

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
  cell. **Not achieved yet:** the amp and flash are powered down between plays, but MCU
  standby is unimplemented, so bench idle is the MCU's poll loop — a few mA. The battery
  life claim rests entirely on that one missing piece.
- Average draw while playing is nothing like the 200 mA peak: 8 kHz speech at ~10 dB
  crest factor spends most of its time far below peak, and a play lasts 10 s. Battery
  life is an idle-current problem, not a playback-energy problem.

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
| 2 | PD5 | **not wired** (USART1 TX if ever needed) | was printf + UART loader on the perfboard |
| 3 | PD6 | **not wired** (USART1 RX if ever needed) | was the UART loader's RX |
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

**On PD5/PD6.** The perfboard wired them as USART1 for printf and for the original UART
chant loader (route C). The current board does not, and the schematic labels them
`USB_P`/`USB_N` — a label to fix, because **the CH32V003F4P6 has no USB peripheral**.
Nothing in the firmware needs them now; `src/loader/loader_swio.c` carries the audio
over SWIO instead. Leave them as spare GPIO, or break them out as a 2-pin test header
if you ever want route C back.

**J1 programming header:** 3 pins — 3 V, SWIO (PD1), GND. Two rules learned the hard
way: never reconfigure PD1 as ordinary GPIO in firmware (you can lock the programmer
out and need the WCH tool's power-cycle unlock), and always share ground with the
programmer. Which LinkE pin goes where is the next section.

## The WCH-LinkE — pins and how to hook it up

This is the only programmer this project needs, and it is also the USB-serial adapter
the audio loader uses. Those are **two different interfaces on one dongle** and they are
wired to different MCU pins; see [`bring-up.md`](bring-up.md) stage 2 and 5b.

### Read the labels, do not count the pins

The LinkE's pin header is silkscreened with signal names (`SWDIO`/`DIO`, `SWCLK`/`CLK`,
`3V3`, `GND`, `TX`, `RX`, `RST`, `5V`). **Wire by those names.** The physical order
differs between WCH-LinkE board revisions and differs again on the clones, so a pin
position copied from someone else's photo or blog is the one thing here that will bite
you. WCH's own manual gives the signals, never a position.

### Flashing — 3 wires to J1

The CH32V003 debugs over **single-wire** SWIO, not 2-wire ARM SWD. WCH's manual lists
CH32V003 as `SWDIO = PD1`, `SWCLK = –` — the dash is not an omission, there is no clock
line. So J1 is 3 pins and nothing more:

| WCH-LinkE | J1 / U1 | Note |
|---|---|---|
| `SWDIO` (`DIO`) | J1 SWIO → U1 pin 18 **PD1** | the whole debug link, data and clock in one wire |
| `GND` | J1 GND | **never optional** — see below |
| `3V3` | J1 3 V | only when the CR123A is out; SW2 off |
| `SWCLK` | *(nothing)* | not used by this chip; leave it unconnected |
| `RST` | *(nothing)* | NRST/PD7 has R5 to the rail; the probe does not need it |

Then:

```sh
pio run -e player -t upload     # flash the player over SWIO
```

**Reading printf on a three-wire board is an open problem.** `[env:player]` sends printf
to PD5 at 115200, which needs a wire this board does not have; the Makefile build sends
it over SWIO instead, read with `minichlink -T`, which cannot open this probe. The
candidate is `wlink sdi-print`, WCH's own SWIO console — **untested here**, and whether
it agrees with ch32fun's debug-printf framing is unverified. Until someone tries it,
treat the player's boot log as unavailable on this board and lean on the loader's JEDEC
check and the CRC32 instead. Tracked in [`decisions.md`](decisions.md).

Make equivalent, for the reference build: `make flash` — but on Windows it goes through
`minichlink`, which does not work here at all; see
[Which tool can actually drive the probe](#which-tool-can-actually-drive-the-probe).

Three rules, all of them learned the hard way somewhere:

- **Share ground first.** Without a common GND the SWIO level has no reference and the
  dongle either fails to find the chip or finds it intermittently, which reads exactly
  like a bad solder joint.
- **One power source at a time.** 3V3 from the LinkE *or* the CR123A through SW2, never
  both — back-feeding the dongle's regulator from a lithium primary is how you lose the
  dongle. Pre-power checklist in [`bring-up.md`](bring-up.md) has this as a tick box.
- **The LinkE must be in RISC-V mode**, not ARM/SWD mode. The blue LED is the tell: in
  RISC-V mode it is **off when idle** and flashes while downloading; in ARM mode it sits
  **on**. Switch it by holding the **ModeS** button while plugging the dongle in
  (WCH-LinkE-R0-1v2 and later), or from WCH-LinkUtility / MounRiver Studio. A dongle in
  the wrong mode simply does not see the chip.

**Never reconfigure PD1 as ordinary GPIO in firmware.** You can lock the programmer out
of the chip; the way back is a power-cycle unlock — `wlink erase --method power-off`
on this machine, since `minichlink -u` cannot open the probe. This is also why PD1
carries no other function in `board.h`.

### Loading audio — the same three wires

**Nothing else is needed. This is the milestone of 2026-10-23: a board was programmed
end to end over one signal wire.**

```sh
python tools/load_audio_swio.py        # loader -> MCU, audio -> U3, player -> MCU
```

~40 s for the 82 KB chant, ending in a CRC32 the MCU computes over the whole image and
the host checks. `src/loader/loader_swio.c` is flashed in place of the player, takes the
audio through a RAM mailbox over the debug link, writes it to U3 over SPI, and the player
goes back on at the end — all three steps in that one command. How the mailbox works and
why: [`audio-pipeline.md`](audio-pipeline.md) route D.

<details>
<summary>Route C — the old UART loader, for a board that does wire PD5/PD6</summary>

The dongle also exposes a USB-CDC serial port, which `send_chant.py` talks to. Labels
below are **MCU-side**, so TX/RX cross:

| WCH-LinkE | U1 | |
|---|---|---|
| `RX` | pin 2 **PD5** | MCU TX → adapter RX |
| `TX` | pin 3 **PD6** | MCU RX ← adapter TX |
| `GND` | VSS | again, not optional |

**57600 8N1**, which is `LOADER_BAUD` in `board.h` and `send_chant.py`'s default —
halved from 115200 because on hand-wiring the faster rate corrupted bytes faster than
the resync machinery could recover (`debug-log.md` §2). Do not confuse it with the
**115200** the *player's* UART printf uses on the same PD5 wire: two firmwares, two
rates, one pin.

In PlatformIO the two interfaces are two different settings and mixing them up produces
a confusing "port not found": **`upload_port` is the debug probe, `custom_chant_port` is
the CDC port.** On Windows only the CDC side gets a `COMx` number.

This route still works and is still tested in the tree. It is simply not what this board
needs.

</details>

### Which wires does each step need?

All of them, the same three, for every step:

| Step | Command | Interface | Wires |
|---|---|---|---|
| loader → MCU | (inside `load_audio_swio.py`) | SWIO | SWDIO→PD1, GND, 3V3 |
| audio → U3 | (inside `load_audio_swio.py`) | SWIO | the same three |
| player → MCU | (inside `load_audio_swio.py`) | SWIO | the same three |

That is the whole point of route D, and it is why the four remaining boards are one
command each with nothing to re-plug.

**You only need to load a chant once per board.** U3 is non-volatile and the player
never writes it, so the chant survives every reflash of the MCU. Day-to-day firmware
iteration — new player build, `pio run -e player -t upload`, press the switch — never
touches it.

### Which tool can actually drive the probe

**Measured on this project's Windows machine, 2026-10-23.** This matters more than it
looks, because the docs used to hand out `minichlink` commands freely:

| Tool | Opens this probe? | |
|---|---|---|
| OpenOCD (`tool-openocd-riscv-wch`) | **yes** | what `pio ... -t upload` uses, and `swio_link.py`'s default backend |
| `wlink` (`tool-wlink`) | **yes** | WCH's own CLI; `wlink erase --method power-off` is the unbrick |
| `minichlink` | **no** | `libusb_open() = -5` (`LIBUSB_ERROR_ACCESS`) |

The LinkE enumerates as `VID_1A86&PID_8010` (`WCH-LinkRV`, RISC-V mode) bound to **WCH's
own driver**. OpenOCD and `wlink` speak to that driver; `minichlink` wants a WinUSB
binding and cannot open the device at all. **So no `minichlink` command in this tree can
be assumed to run here** — not `minichlink -T` for printf, not `minichlink -u` to
unbrick. Where this file used to say `minichlink -u`, it now says
`wlink erase --method power-off`. Full write-up: [`debug-log.md`](debug-log.md) #6.

### Windows: the driver, and why PlatformIO is the easier route

`minichlink` talks to the LinkE through libusb and needs the dongle's interface bound
to **WinUSB** — use [Zadig](https://zadig.akeo.ie/) if it reports that it cannot find the
programmer. PlatformIO's upload path brings its own driver handling and sidesteps that.

Measured on this machine, 2026-10-23, with the LinkE attached and healthy:

```
> minichlink -i
Error: couldn't open wch link device (libusb_open() = -5)
Error: Could not initialize any supported programmers
```

```
> openocd -f wch-riscv.cfg -c "init"
Info : WCH-LinkE  mode:RV version 2.20
```

The probe enumerates as `VID_1A86&PID_8010` (`WCH-LinkRV`, RISC-V mode) under WCH's own
driver. OpenOCD and `wlink` speak to that driver; minichlink does not, and `-5` is
`LIBUSB_ERROR_ACCESS` — the binding, not the cable. **So anything in this tree that
needs a probe must be able to go through OpenOCD**, which is why
`tools/swio_link.py` has an OpenOCD backend and defaults to it. Do not assume a
`minichlink` command in any doc will run here.

**This is not hypothetical: `make flash` has not worked on this project's Windows
machine, and PlatformIO is what every successful flash here has used.** So the `pio`
commands lead throughout these docs and the Makefile ones are given as the
cross-reference. If you hit the same wall, reach for `pio run -e player -t upload` rather
than spending the session on Zadig — the Makefile and PlatformIO flash the same bytes
over the same SWIO wire either way, so nothing is lost by taking the route that works.

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

   **This is why loudness turned out to be a file problem, not a circuit problem.** 0.32
   against 0.37 Vpp means the analog chain is already running flat out; raising R3 to lift
   the ratio only pushes the amp into clipping. When the chant came out too quiet, the
   headroom being wasted was *inside the recording* — peaks at full scale, RMS 13.7 dB
   below — and the fix was a look-ahead limiter in `tools/raw_resample.py`, not a
   resistor. See [`audio-pipeline.md`](audio-pipeline.md).
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

Streaming needs only 64 kbit/s against a 50 MHz-capable bus — enormous slack. The bus is
idle almost all the time. The firmware clocks it at **3 MHz** (`FLASH_SPI_BR = 3` in
`board.h`), which is still ~50× the required rate; the margin is for hand-wired
perfboard, where long unshielded SCK/DO runs are what actually breaks. If the JEDEC ID
reads back intermittently wrong, slow it down before suspecting the solder joints.

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

Reference designators are easy to transpose, so to be explicit: **R4 is the 100 kΩ
pull-down on the amp's SD pin; R5 is the 10 kΩ pull-up on NRST.** Some older comments in
`src/` had these the other way round.

A 10 kΩ pull-up on CS# (PC4) is optional and is not in the table above. It keeps the
flash deselected while the MCU's pins float during reset or reprogramming;
`spiflash_init()` already raises CS in the output register before making the pin an
output, so firmware does not depend on it. Fit it if you have the space.

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

## Tools

- **WCH-LinkE** — acquired and in use. It is the programmer (SWIO on PD1) *and* the
  USB-serial adapter the *old* UART loader used (its CDC port on PD5/PD6). Those are two
  different interfaces on one dongle; PlatformIO's `upload_port` means the first,
  `custom_chant_port` the second. Pin-by-pin wiring for both: [The WCH-LinkE — pins and
  how to hook it up](#the-wch-linke--pins-and-how-to-hook-it-up) above.
- A CH341A USB programmer is optional — useful only while the flash is on a DIP adapter,
  in which case it writes `chant8k-limit.bin` directly (header included) and the loader
  firmware is not needed at all.
- March has DediProg DP3000 access at work for batch-programming SPI flash; whether
  FM25Q08B is in its device list is still unconfirmed.

## What is still to build

- **MCU standby.** The circuit supports it (PD3 is EXTI-capable, the amp and flash both
  have real off states); the firmware does not implement it. Everything about the battery
  life claim depends on this.
- **PCB.** Not started — KiCad, JLCPCB. Perfboard is what exists.
- **Enclosure.** Not started. The CR123A's 17 × 34.5 mm is the real constraint on the
  30 mm target; verify the stack-up with the 28 mm driver and the key switch before
  committing CAD.
