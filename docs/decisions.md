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

## Decided during firmware bring-up

| Decision | Reasoning |
|---|---|
| **Sample tick is SysTick**, not a TIM1 update divisor | 48 000 000 / 8000 = 6000 exactly, so the carrier stays at exactly 187.5 kHz and the sample rate at exactly the header's rate. 187500 / 8000 = 23.4375 has no whole-number answer. Proved on the LED demo, adopted in `src/audio.c`. |
| **Press during playback is ignored** | `play_chant()` blocks. A chant that restarts halfway through is worse than one that finishes. Change it by polling PD3 inside the service loop if the monk disagrees. |
| **Sample rate comes from the flash header**, not a compile-time constant | Re-recording at a different rate needs no firmware change. `board.h`'s 8000 is only the default the host tool writes. |
| **Loader verifies with a CRC32 computed on the MCU** | Reading 1 MB back over the UART costs as long again as writing it. Four bytes instead, same polynomial as `zlib.crc32`. |
| **Loader pages are a fixed 256 bytes**, host pads with 0xFF | No length field to get wrong on either side, and 0xFF is the erased state so the padding costs no wear. |
| **Protocol integers are big-endian**, header fields little-endian | Big-endian matches the order the flash wants its addresses in. The header is payload, not protocol, and was already specified little-endian. |

## Open

- **MCU standby (build step 7).** Not implemented. Amp and flash are already in their
  < 1 µA states between plays; the MCU's poll loop is what remains. Left out
  deliberately rather than guessed at, because standby changes how the debug interface
  behaves and a wrong first attempt looks like a dead board. `minichlink -u` is the
  escape hatch. This is the last thing between the current firmware and the design's
  battery life.
- **Press during playback:** currently ignored (see above). Worth asking mom / the monk
  whether a fidget toy should restart instead.
- **Raw PCM vs IMA ADPCM:** depends on the length of the chosen chant. Start raw; switch
  only if it does not fit in ~2 minutes.
- **Enclosure:** ~30 mm diameter target, height flexible, keychain form factor. March is
  willing to go bigger than 30 mm if the CR123A + 28 mm driver + key switch stack
  demands it — a CR123A is 17 × 34.5 mm, which is the real constraint, so verify the
  stack-up before committing the CAD. Printing on a Qidi Q2 Combo.
- **Battery access:** the enclosure needs a way to change the cell without breaking glue.
- **`tools/wav2raw.py` is still a stub.** `ffmpeg -f u8 -ar 8000 -ac 1` does the same
  job today and `tools/send_chant.py` builds the header, so nothing is blocked. What
  the stub would add is the ~200 Hz high-pass and peak normalisation, which the driver
  wants and ffmpeg will not do for you.
- **IMA ADPCM is not implemented on the MCU.** `audio_start()` refuses format 1 rather
  than playing noise. Only needed if the chosen chant exceeds ~2 minutes.
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
