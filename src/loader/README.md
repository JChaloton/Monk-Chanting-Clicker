# Loader firmware

A one-off firmware, flashed **instead of** the player, that receives the audio blob
over UART (PD5 TX / PD6 RX) and writes it into U3 page by page. After the load, flash
the real player firmware back.

This is route C in [`../../docs/audio-pipeline.md`](../../docs/audio-pipeline.md).
Routes A (CH341A with the flash on a DIP adapter) and B (DediProg at work) need no
firmware at all and are easier while the chip is socketed. Once the flash is soldered
down on a perfboard or a PCB, this is the route.

## Use

```sh
make -f Makefile.loader flash                 # loader onto the MCU
python tools/send_chant.py COM7 chant.raw     # ~2 min for a full 1 MB
make flash                                    # player back onto the MCU
```

Wiring — the header labels are from the MCU's point of view, so TX/RX cross:

| ISP header | MCU | USB-serial adapter |
|---|---|---|
| TX | PD5 (pin 2) | RX |
| RX | PD6 (pin 3) | TX |
| GND | — | GND |

A WCH-LinkE works: it is a debug probe *and* a USB-serial adapter, and its serial pins
are the ones to use here. **Do not power the board from both the adapter and the
CR123A** — pick one, or open the master switch.

## Protocol

One ASCII byte per command, arguments after it, every reply starting with a status
byte: `K` ok, `E` flash timed out, `C` checksum mismatch. Multi-byte integers on the
wire are **big-endian**, matching the order the flash chip wants its addresses in.
(The little-endian fields inside the 16-byte header are payload, not protocol.)

| Command | Reply | Meaning |
|---|---|---|
| `?` | `K` `"CL01"` | ping and version — also how the host resyncs |
| `I` | `K` id2 id1 id0 | JEDEC ID, must be `A1 40 14` |
| `U` | `K` \| `E` | clear the status-register block-protect bits |
| `S` a2 a1 a0 | `K` \| `E` | erase one 4 KB sector |
| `P` a2 a1 a0 &lt;256 B&gt; sum | `K` \| `C` \| `E` | program one page |
| `X` a2 a1 a0 n3 n2 n1 n0 | `K` c3 c2 c1 c0 | CRC32 over a range |

`P` always carries exactly 256 bytes; the host pads the last page with `0xFF`, which is
the erased state, so the padding costs nothing. `sum` is the low byte of the sum of the
three address bytes plus all 256 data bytes — a dropped byte on a hand-wired UART with
no flow control is rare but real, and a silent bad byte in the middle of a chant is very
hard to diagnose by ear.

`X` is why verification is cheap. Reading the whole chip back over the UART would take
as long as writing it; instead the MCU computes the CRC32 itself and sends four bytes.
Same polynomial as `zlib.crc32`, so `tools/send_chant.py` compares directly.

## Design notes

- The MCU is a pure responder. The host waits for each reply before sending the next
  command, so the UART receive path can be blocking with no timeouts and no state
  machine to get out of sync.
- Unknown bytes are ignored rather than answered. Line noise at connect time must not
  desynchronise anything; the host resyncs by sending `?` until it gets `K`.
- `PC3` (amp enable) is driven LOW first thing and never touched. A loader that made
  noise while it worked would also put a couple of hundred milliamps of class-D
  switching right next to the SPI bus.
- `spiflash_wake()` runs at startup, because the player leaves the chip in deep
  power-down and a sleeping chip ignores every command — which looks exactly like
  broken wiring.
