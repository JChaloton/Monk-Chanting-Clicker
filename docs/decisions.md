# Decisions and open questions

## Decided

| Decision | Reasoning |
|---|---|
| Switch signals the MCU; it does not gate power or audio | Enables debounce, full-chant playback from a short press, amp shutdown between plays, µA sleep. A switch wired straight to a sound module can do none of that. |
| CR123A primary cell, no regulator, one 3 V rail | All three ICs accept 3 V. Rechargeable ruled out on cost — LIR2450 ≈ 150–200 THB each in Thailand, and a charger circuit adds parts. User-replaceable is better for a gift. |
| PAM8302A instead of LM386 | Class-D efficiency, 2.0 V floor, active-low shutdown pin (the thing that makes years of standby possible), tiny package. LM386 was the earlier plan. |
| External SPI flash (FM25Q08B) for audio | The MCU's 16 KB would hold about two seconds of sound. 1 MB holds a whole chant with room to spare, and MCU flash stays code-only. |
| 8-bit PWM at 187.5 kHz, 8 kHz sample rate | No DAC on the CH32V003. 48 MHz / 256 gives an 8-bit PWM directly; 8 kHz suits a voice and the 700 Hz–5 kHz driver. |
| Volume set by hardware divider, samples kept full-scale | Software attenuation throws away resolution and adds hiss. |
| 28 mm HDK-2808BB-7 driver | 8 Ω / 1.5 W is 3× the amp's real output at 3 V, so nothing burns out; 97 dB is loud enough; its band matches speech. |
| Write the firmware by hand (ch32v003fun), not a canned sound module | This project is explicitly a learning vehicle across CAD, circuit design, and firmware. |

## Open

- **Press during playback:** ignore it, or restart the chant? Affects the ISR/state
  machine. Worth asking mom / the monk which feels right for a fidget toy.
- **Raw PCM vs IMA ADPCM:** depends on the length of the chosen chant. Start raw; switch
  only if it does not fit in ~2 minutes.
- **8 kHz tick source:** 187500 / 8000 = 23.4375, so a whole-number TIM1 update divisor
  does not land exactly on 8 kHz. Either use a second timer for the sample tick, or
  shift the carrier slightly. Decide during firmware step 3.
- **Enclosure:** ~30 mm diameter target, height flexible, keychain form factor. March is
  willing to go bigger than 30 mm if the CR123A + 28 mm driver + key switch stack
  demands it — a CR123A is 17 × 34.5 mm, which is the real constraint, so verify the
  stack-up before committing the CAD. Printing on a Qidi Q2 Combo.
- **Battery access:** the enclosure needs a way to change the cell without breaking glue.
- **WCH-LinkE:** not yet acquired. Blocks all firmware work.
- **FM25Q08B in the DediProg DP3000 device list?** Confirm before assuming the work
  programmer can batch-write these.
- **PCB:** not started. KiCad, JLCPCB when it happens. Breadboard first.

## Context worth keeping

- Recipient: a Buddhist monk; the clicker is a gift from March's mother, who requested
  it and chose the chant. A blue (clicky) switch was chosen for the tactile click.
- A physical master power switch (SW2) is deliberate — the electronics sleep at µA
  anyway, but a monk may want guaranteed silence, and it is kind to whoever changes the
  battery.
- Parts are sourced from LCSC; boards from JLCPCB; March has ordered from both.
- There is a possible product angle later (Shopee), plus a short-form video about the
  build.
