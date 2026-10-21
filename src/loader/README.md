# Loader firmware

A one-off firmware, flashed **instead of** the player, that receives the audio blob over
UART (PD5 TX / PD6 RX) and writes it into U3 page by page. After the load, flash the real
player firmware back.

This is route C in [`../../docs/audio-pipeline.md`](../../docs/audio-pipeline.md).
Routes A (CH341A with the flash on a DIP adapter) and B (DediProg at work) need no
firmware at all and are easier while the chip is socketed. Once the flash is soldered
down on a perfboard or a PCB, this is the route — and it is the one actually used.

## Use

One command runs all three steps and puts the player back even if the transfer fails:

```sh
python tools/load_audio.py --pio                       # port auto-detected
python tools/load_audio.py --pio other.raw --port COM9
```

By hand, which is the same three steps:

```sh
pio run -e loader -t upload                        # loader onto the MCU
pio run -e loader -t sendchant                     # ~16 s for the 82 KB chant
pio run -e player -t upload                        # player back onto the MCU
```

With Make instead — `make load`, or `make -f Makefile.loader flash` /
`python tools/send_chant.py COM7 chant8k-limit.raw` / `make flash`. `--pio` is what
switches `load_audio.py`'s two flashing steps to PlatformIO; without it it shells out to
the Makefiles.

Wiring — the header labels are from the MCU's point of view, so TX/RX cross:

| ISP header | MCU | USB-serial adapter |
|---|---|---|
| TX | PD5 (pin 2) | RX |
| RX | PD6 (pin 3) | TX |
| GND | — | GND |

A WCH-LinkE works: it is a debug probe *and* a USB-serial adapter, and its CDC serial
interface is the port to use here — not the SWIO debug interface. **Do not power the
board from both the adapter and the CR123A** — pick one, or open the master switch.

**57600 baud, 8N1.** That is `LOADER_BAUD` in `src/board.h` and the default in
`send_chant.py`; change one and you must change the other, or the loader will not answer
at all. It was 115200 and was halved: the HSI is only ~1 % accurate and on hand-wiring
the faster rate corrupted bytes faster than the resync machinery could recover.

## Protocol

One ASCII byte per command, arguments after it, every reply starting with a status byte:
`K` ok, `E` flash timed out (or a missing operand), `C` checksum mismatch or short read.
Multi-byte integers on the wire are **big-endian**, matching the order the flash chip
wants its addresses in. (The little-endian fields inside the 16-byte header are payload,
not protocol.)

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
three address bytes plus all 256 data bytes — a dropped byte on a hand-wired UART with no
flow control is rare but real, and a silent bad byte in the middle of a chant is very
hard to diagnose by ear.

`X` is why verification is cheap. Reading the whole chip back over the UART would take as
long as writing it; instead the MCU computes the CRC32 itself and sends four bytes. Same
polynomial as `zlib.crc32` (CRC-32/IEEE, reflected, `0xEDB88320`), so
`tools/send_chant.py` compares directly. It is computed bitwise: a 1 kB table would be 4×
faster and would eat half the chip's 2 KB of RAM, and 1 MB still takes only a few seconds
at 48 MHz.

A full session, as the host drives it: `?` → `I` → `U` → one `S` per 4 KB sector → `?` →
one `P` per 256 B page → `X` over the whole padded image.

## Design notes

- **The MCU is a pure responder.** The host waits for each reply before sending the next
  command, so the receive path can be blocking with no state machine to get out of sync.
- **Every operand read is bounded at 50 ms; the command read is not.** Waiting for the
  next command is the idle state, so `uart_rx()` blocks forever on purpose. But once an
  opcode has arrived, a byte that never turns up must not park the loader mid-command —
  see the failure mode below. Any short read is answered `C` (for `P`) or `E` (for `S`
  and `X`), statuses the host already knows how to retry.
- **Why 50 ms specifically.** It sits between the two timescales that matter: far above
  the ~174 µs gap between bytes of a back-to-back burst at 57600 (so a healthy transfer never trips
  it, and USB's 1 ms frame scheduling cannot either), and far below the host's 3 s status
  timeout (so the loader is back at the command loop, having answered, long before the
  host gives up). Because it only fires after 50 ms of silence, the host has demonstrably
  stopped sending — so no stray page bytes are left in flight to be misread as commands.
- **Unknown bytes are ignored rather than answered.** Line noise at connect time must not
  desynchronise anything; the host resyncs by sending `?` until it gets `K`.
- **`PC3` (amp enable) is driven LOW first thing and never touched.** A loader that made
  noise while it worked would also put a couple of hundred milliamps of class-D switching
  right next to the SPI bus.
- **`spiflash_wake()` runs at startup**, because the player leaves the chip in deep
  power-down and a sleeping chip ignores every command — which looks exactly like broken
  wiring.

### The failure mode the 50 ms bound defends against

This actually happened, and it is worth knowing because the symptom does not look like
its cause. With unbounded operand reads, a single byte lost to noise left the loader
waiting for a byte the host had already finished sending, while the host waited for a
status byte that would never come. Both ends wedged. Then the host's `?` resync probes
were consumed **as operand bytes** — and three of them complete a 24-bit address, which
is a real `S` sector erase, answered with a genuine `K`. One dropped byte turned into a
storm of spurious erases and replies the host could not distinguish from real ones.

Returning to the command loop on silence is what makes `?` mean `?` again. Full account:
[`../../docs/debug-log.md`](../../docs/debug-log.md) §2.

## Host side

`tools/send_chant.py` carries the matching discipline, and two of its details are
load-bearing rather than stylistic:

- It **never reassigns `ser.timeout`**. On Windows that setter reconfigures the open
  port, and reconfiguring mid-transaction discards bytes written but not yet clocked out
  — truncating the 261-byte page just sent, so the loader answers `C` to a page the host
  built perfectly. This was the bug that blocked the whole transfer before it was found.
- `_drain()` reads until the line has been **silent**, rather than flushing once. The
  USB-serial bridge buffers, so a slow command can still be delivering replies long after
  you gave up on it. `sync()` sends one `?` per attempt for the same reason: a burst
  queues replies that desynchronise the next real command.

Erases and pages each retry (`--retries`, default 3) with a `sync()` between attempts,
and there is an explicit resync at the erase→program boundary — page 0 carries the `CHNT`
header the player looks for, and it is the worst place to discover the link is out of
step.
