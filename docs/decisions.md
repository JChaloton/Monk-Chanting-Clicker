# Decisions and open questions

The reasoning behind the design, so none of it has to be re-derived. Anything reversed
later is kept with the reversal noted, because the reason it was wrong is usually more
useful than the decision itself.

## Decided — design

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
| **Sample tick is TIM2** — not a TIM1 update divisor, and no longer SysTick | 48 000 000 / 8000 = 6000 exactly (`ATRLR = 5999`), so the carrier stays at exactly 187.5 kHz and the sample rate at exactly the header's rate; 187500 / 8000 = 23.4375 has no whole-number answer. **Reversed from SysTick after first hardware bring-up:** ch32fun builds `Delay_Us()`/`Delay_Ms()` on the free-running SysTick counter, so reprogramming SysTick broke every delay, and stopping it with `CTLR = 0` froze `CNT` so the next delay never returned. The board hung inside `audio_stop()`'s anti-pop ramp with the amp still enabled: one press, then hiss forever. TIM2 divides the same clock just as exactly and leaves the timebase alone. |
| **SysTick stays free as an independent measurement clock** | A consequence of the above worth stating on its own: `play_range()` times playback against SysTick, so a TIM2 misconfiguration cannot hide by skewing its own measurement. This is what proved the firmware innocent when the chant sounded wrong. |
| **Press during playback is ignored** | `play_chant()` blocks. A chant that restarts halfway through is worse than one that finishes. Change it by polling PD3 inside the service loop if the monk disagrees. |
| **Sample rate comes from the flash header**, not a compile-time constant | Re-recording at a different rate needs no firmware change. `board.h`'s 8000 is only the default the host tool writes. |
| **Loader verifies with a CRC32 computed on the MCU** | Reading 1 MB back over the UART costs as long again as writing it. Four bytes instead, same polynomial as `zlib.crc32`. Bitwise, not table-driven: RAM is 2 KB and a few seconds is fine. |
| **Loader pages are a fixed 256 bytes**, host pads with `0xFF` | No length field to get wrong on either side, and `0xFF` is the erased state so the padding costs no wear. |
| **Protocol integers are big-endian**, header fields little-endian | Big-endian matches the order the flash wants its addresses in. The header is payload, not protocol, and was already specified little-endian. |
| **Every loader operand read is bounded at 50 ms** | Unbounded reads let one dropped byte park the loader mid-command, where the host's `?` resync probes were consumed as operand bytes — three of them complete an address and trigger a real sector erase that answers `K`. 50 ms sits between the 87 µs inter-byte gap and the host's 3 s status timeout. See `debug-log.md` §2. |
| **`LOADER_BAUD` 57600, not 115200** | The HSI is ~1 % accurate and the link is hand-wired. At 115200 it corrupted bytes faster than the resync machinery could recover. 450 KB still loads in ~86 s. |
| **The host never reassigns `ser.timeout`** | On Windows that setter reconfigures the open port, discarding bytes written but not yet clocked out — truncating the page just sent, so the loader answered `C` to perfectly built pages. `debug-log.md` §1. |
| **Loudness is fixed in the file, with a look-ahead limiter** | R2/R3 already run the amp's input flat out at ~0.32 Vpp against a ~0.37 Vpp ceiling, so nothing in the schematic can help. The chant peaks at full scale but averages 13.7 dB down. `--limit 16` closes that to 10.3 dB with peaks in the same place, so the amp never clips harder. **Confirmed by ear on hardware; this is the setting that ships.** |
| **Look-ahead in the limiter is mandatory, not a refinement** | Without it, measured, the crest factor got *worse* (13.7 → 14.9 dB): the gain has not finished falling when the transient passes, so peaks survive while sustained material is pulled down. 2 ms of look-ahead gives 10.3 dB. |
| **Bring-up diagnostics are kept, switched off** | `PLAYER_SELFTEST` guards a TIM1-only test tone (halves the chain into analog vs upstream) and a rate sweep (replays flash at candidate rates instead of an 80-second reload per guess). Both earned their keep; both cost nothing while off. |
| **`tools/load_audio.py` wraps the three-step load** | Loader → audio → player, with the player reflashed even when the transfer fails. A board left holding the loader is a board that sits silent while you wonder why the button does nothing. |

## Decided while making the board a three-wire board (2026-10-23)

| Decision | Reasoning |
|---|---|
| **The chant loads over SWIO, not UART** (`loader_swio.c`, route D) | The board wires nothing to PD5/PD6, and the schematic labels them `USB_P`/`USB_N` on a part with **no USB peripheral**. Rather than add a serial adapter, the loader takes the audio over the one wire that already programs the chip. Three wires now do the whole pipeline. Route C is kept and still works for a board that does wire PD5/PD6. |
| **A RAM mailbox, not a byte stream over the debug channel** | ch32fun's SWIO printf channel is the obvious transport and is unusable twice over: three bytes per USB round trip, and on Windows minichlink reads the host end with `_kbhit()`/`_getch()`, which only see a real console. The debug module's ordinary memory access moves 512 bytes per operation instead. |
| **The mailbox lives at a fixed `0x20000300`** | The host must find it without parsing the ELF. It sits above .data/.bss (256 B) and below the stack; `main()` checks the boundary at boot and hangs deliberately rather than corrupting itself, so the failure is a clear host-side timeout. |
| **OpenOCD is the default backend, not minichlink** | Measured: `minichlink` cannot open this machine's probe at all (`libusb_open() = -5`); the LinkE is bound to WCH's own driver, which OpenOCD and `wlink` speak. It is also far faster — one process for the whole load instead of one per chunk. `debug-log.md` #6. |
| **The per-chunk ack is mandatory on a fast backend** | Not an optimisation to remove. What matters between chunks is how long the core is *running*, and nearly all of an OpenOCD chunk is spent halted — a few milliseconds of run time against up to 6 ms of page programming. Skipping it produced a clean run that failed the final CRC32. `debug-log.md` #7. |
| **`loader_swio` builds with printf compiled out** | ch32fun's SWIO printf uses the debug module's data registers — the same channel the loader's host is driving. Two owners, one channel. |

## Open

- **MCU standby (build step 7).** Not implemented. Amp and flash are already in their
  < 1 µA states between plays; the MCU's poll loop is what remains, a few mA. Left out
  deliberately rather than guessed at, because standby changes how the debug interface
  behaves and a wrong first attempt looks like a dead board. A power-cycle unlock is the
  escape hatch — `wlink erase --method power-off`, since `minichlink -u` cannot open this
  probe. **This is the last thing between the current firmware and the design's battery
  life**, and the only reason this is a bench project rather than a finished one.

- **Press during playback:** currently ignored (see above). Worth asking mom / the monk
  whether a fidget toy should restart instead.
- **Raw PCM vs IMA ADPCM:** moot for this chant (10.25 s = 8 % of the chip). Only
  revisit if a future chant exceeds ~2 minutes. `audio_start()` refuses format 1 today
  rather than playing noise.
- **Enclosure:** ~30 mm diameter target, height flexible, keychain form factor. March is
  willing to go bigger than 30 mm if the CR123A + 28 mm driver + key switch stack demands
  it — a CR123A is 17 × 34.5 mm, which is the real constraint, so verify the stack-up
  before committing the CAD. Printing on a Qidi Q2 Combo.
- **Battery access:** the enclosure needs a way to change the cell without breaking glue.
- **No verified way to read the player's `printf` on a three-wire board.** PlatformIO's
  `[env:player]` sends it to PD5, which is not wired; ch32fun's SWIO printf is read with
  `minichlink -T`, which cannot open this probe. `wlink sdi-print` is WCH's own SWIO
  console and is the obvious candidate — **untested**, and whether it agrees with
  ch32fun's debug-printf framing is unknown. Until someone tries it, the loader's JEDEC
  check and the CRC32 are the only on-board diagnostics.
- **`tools/wav2raw.py` is still a stub**, and arguably should just be deleted. `ffmpeg`
  plus `tools/raw_resample.py` does everything it was specified to do, including the
  high-pass and normalisation that were its reason for existing, and `send_chant.py`
  writes the header. The only thing its docstring still carries that exists nowhere else
  is the intended ADPCM encoder layout. Note it is also the only tool that writes a
  headered `.bin` from a `.wav`, which route A/B (an external programmer) needs — but it
  has no `--limit`, so it cannot produce the loudness the shipping chant uses.
- **FM25Q08B in the DediProg DP3000 device list?** Confirm before assuming the work
  programmer can batch-write these.
- **PCB:** not started. KiCad, JLCPCB when it happens.

## Context worth keeping

- Recipient: a Buddhist monk; the clicker is a gift from March's mother, who requested it
  and chose the chant. A blue (clicky) switch was chosen for the tactile click.
- The chant is `NaKomNamo`, 10.25 s.
- A physical master power switch (SW2) is deliberate — the electronics sleep at µA
  anyway, but a monk may want guaranteed silence, and it is kind to whoever changes the
  battery.
- Parts are sourced from LCSC; boards from JLCPCB; March has ordered from both.
- There is a possible product angle later (Shopee), plus a short-form video about the
  build.
