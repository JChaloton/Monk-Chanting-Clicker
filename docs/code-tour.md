# Code tour — every file, what it does, and why it is shaped that way

This is the document to read first when you come back to this project cold. It walks
the tree in the order the bytes actually flow: recording → host tool → debug link →
SPI flash → MCU RAM → PWM → speaker. Where a line of code is non-obvious, the reason it is that
way is here, not only in the source.

Companions: [`hardware.md`](hardware.md) for the circuit,
[`firmware-plan.md`](firmware-plan.md) for the design targets and register maths,
[`debug-log.md`](debug-log.md) for the bugs that shaped the current code.

---

## The 60-second version

Two separate firmware images are built from one `src/` tree. Only one can be on the
MCU at a time, and knowing which one is on there is most of the operational complexity
of this project.

| Firmware | Sources | Built by | What it is for |
|---|---|---|---|
| **player** | `main.c` `audio.c` `spiflash.c` | `pio -e player` / `make` | the product: press switch → chant |
| **loader_swio** | `loader/loader_swio.c` `spiflash.c` | `pio -e loader_swio` / `make -f Makefile.loader-swio` | one-off: host → SWIO → SPI flash. **The one in use.** |
| **loader** | `loader/loader.c` `spiflash.c` | `pio -e loader` / `make -f Makefile.loader` | one-off: host → UART → SPI flash. Needs PD5/PD6. |

Three images now, not two. `loader_swio` exists because the current board wires nothing
to PD5/PD6: it takes the audio over the same single wire that programs the chip. Both
loaders write U3 through the identical `spiflash.c`; only the transport differs.

Shared by both: `board.h` (pins and timing constants), `funconfig.h` (ch32fun build
config), and `spiflash.c`.

Host-side Python, none of it needed at runtime:

| Tool | Role |
|---|---|
| `tools/load_audio_swio.py` | **the one command you want**: flash loader → send audio over SWIO → flash player back |
| `tools/swio_link.py` | the SWIO half: OpenOCD/minichlink backends and the RAM mailbox |
| `tools/load_audio.py` | the same three steps for route C, over a serial port |
| `tools/send_chant.py` | speaks the UART loader's wire protocol; builds the 16-byte header |
| `tools/raw_resample.py` | fixes a `.raw`'s real sample rate; high-pass, limiter, normalise |
| `tools/wav2raw.py` | **stub**, never finished; ffmpeg + `raw_resample.py` do its job |
| `scripts/pio_send_chant.py` | exposes `send_chant.py` as `pio run -e loader -t sendchant` |

---

## `src/board.h` — the pin contract

Single source of pin truth. Nothing else in the tree may name a port or pin number. If
a pin moves, it moves here and in [`hardware.md`](hardware.md) in the same commit.

The constants that are not pins, and what depends on them:

| Constant | Value | Consumers / consequences |
|---|---|---|
| `AUDIO_SAMPLE_RATE_HZ` | 8000 | **default only.** The player uses the rate from the flash header. This is the number the host tools stamp. |
| `AUDIO_PWM_PERIOD` | 256 | TIM1 `ATRLR = 255`. 48 MHz / 256 = 187.5 kHz carrier, and one 8-bit sample maps straight into `CH1CVR`. |
| `AUDIO_IDLE_DUTY` | 128 | mid-scale. The PWM's resting level is 50 % duty (~1.5 V), not 0 V — the amp input is capacitively coupled and only sees change. |
| `AUDIO_RAMP_MS` | 5 | anti-pop ramp at start and stop. |
| `AUDIO_BUF_BYTES` | 256 | one flash page = 32 ms of audio at 8 kHz. |
| `AUDIO_BUF_COUNT` | 2 | 512 B of the chip's 2 KB of RAM. |
| `FLASH_SPI_BR` | 3 | SPI bit clock = 48 MHz / (2 << 3) = **3 MHz**. Streaming needs 64 kbit/s; the rest is margin for hand-wired perfboard. If the JEDEC ID reads back wrong, **raise this number (slower) before suspecting solder joints.** |
| `LOADER_BAUD` | 57600 | halved from 115200 after the hand-wired link corrupted bytes faster than the resync logic could recover. Must match `send_chant.py --baud`. |
| `SW_DEBOUNCE_MS` | 20 | a blue mechanical switch chatters for a few ms. |

`PD1` is SWIO. Never configure it as GPIO — you can lock the WCH-LinkE out of the chip
and need a power-cycle unlock to get back in (`wlink erase --method power-off`; `minichlink -u` cannot open this machine's probe — `debug-log.md` #6).

## `src/funconfig.h` — ch32fun build configuration

`ch32fun.h` includes `funconfig.h` unconditionally and the framework ships no default,
so every project must supply one. It lives in `src/` because both build systems already
put `src/` on the include path (`-Isrc`), which is what lets `ch32fun.c` find it while
compiling *inside the framework package*.

Three things are decided here:

- **48 MHz from the internal HSI through the 2× PLL.** No crystal, so PA1/PA2 stay free.
  Every audio number descends from this: 48 MHz / 256 = 187.5 kHz carrier,
  48 MHz / 8000 = 6000 for the sample tick.
- **Where `printf` goes.** The ch32fun default is SWIO (PD1), read back with
  `minichlink -T` — no COM port involved, and **not usable on this machine**, where
  minichlink cannot open the probe. The `#ifndef` guards exist so
  that *one* PlatformIO environment can override it, which is what the PlatformIO build
  this project uses does: `[env:player]` passes `-DFUNCONF_USE_UARTPRINTF=1
  -DFUNCONF_USE_DEBUGPRINTF=0`, putting printf on PD5 at 115200 where any USB-serial
  adapter can read it. **The player only**, and only on a board that wires PD5.
  `loader.c` configures USART1 itself at register level (two owners of one peripheral is
  silent corruption, not a link error), and `loader_swio.c` compiles printf out
  entirely, because ch32fun's SWIO printf would contend with the debug channel its own
  host is driving. On the current three-wire board neither printf route reaches
  anything — see [`decisions.md`](decisions.md).
- **`FUNCONF_SYSTICK_USE_HCLK 0`** → SysTick runs at HCLK/8 = **6 MHz**, so
  `DELAY_MS_TIME == 6000`. Any code converting SysTick counts to milliseconds (see
  `play_range()`) depends on that number.

---

## The player

### `src/main.c` — sequencing and diagnostics

`main()` is: init → identify the flash → read and sanity-check the header → sample the
audio itself → put the flash to sleep → poll the switch forever.

The boot log is deliberately verbose, because it is the entire diagnostic surface of a
board with no display:

```
chant-clicker player
flash JEDEC id a14014 (expect a14014)
chant: fmt 0, 8000 Hz, 81968 bytes (~10 s)
samples at 1 s: 7f 80 81 ...  min 96 max 174
switch idle level: HIGH/released (expect HIGH — it is pulled up and pressed = LOW)
ready — press the switch
```

Each line answers one question that has cost time before:

- **JEDEC ID** proves all four SPI wires in both directions at once. `000000` = MISO or
  the chip's 3 V; `ffffff` = CS# never goes low.
- **The header line** proves the load took, and that the rate the player will use is the
  rate you meant. A format other than 0 is refused outright — ADPCM is specified but was
  never written.
- **`samples at 1 s`** proves the *audio* landed, not just the first 16 bytes. Speech
  centres near 128 and swings wide; `min == max` means flat data (0xFF = never written),
  and the chant would play as silence no matter how good the rest of the firmware is.
- **switch idle level** catches a miswired or inverted switch before you start pressing.

`play_range(len, rate)` is the real playback routine; `play_chant()` is just
`play_range(hdr.length, hdr.rate)`. It takes parameters so that bring-up can play a
two-second slice at a candidate rate instead of sitting through the whole chant.

Order of operations, both directions, matters:

```
up:    spiflash_wake() → amp_enable(1) → 5 ms settle → audio_start()
down:  audio_stop() (ramps to mid-scale) → 2 ms → amp_enable(0) → spiflash_deep_power_down()
```

The 2 ms after `audio_stop()` lets the anti-pop ramp reach the amp before the amp is
muted. Muting first would make the ramp pointless.

**The bounded wait.** The service loop is

```c
while (audio_busy()) {
    audio_service();
    if ((uint32_t)(SysTick->CNT - start) > budget) { stalled = 1; break; }
}
```

If the sample interrupt never fires, `running` stays 1 forever and this would spin with
the amp enabled — silence plus hiss and no clue why. The budget is the chant's own
length plus 3 s. It is timed off **SysTick**, which after the TIM2 move is a
free-running counter that the audio path no longer touches — so a TIM2 misconfiguration
cannot hide by skewing its own measurement. That independence is the whole point of
measuring it there.

**Nothing prints during playback.** `printf` blocks for milliseconds and the sample
clock will not wait. Every log line in `play_range()` is emitted after the amp is muted.

**`PLAYER_SELFTEST`** (default 0) re-enables two bring-up aids that both earned their
keep:

- `audio_test_tone(1000, 500)` — 1 kHz straight out of TIM1, no flash and no interrupt
  in the path. It halves the problem: an audible tone means PWM → filter → amp → speaker
  is all good, so a silent chant is upstream of it.
- `rate_sweep()` — replays the same flash bytes at 22050 / 32000 / 44100 / 48000 Hz, two
  seconds each. Reloading 450 KB over the UART to test one guess costs ~80 s; this costs
  two seconds and no reload. This is what found the wrong-rate chant.

**Press during playback is ignored**, because `play_chant()` blocks and the loop then
waits for release plus a debounce. That is a decision, not an accident: a chant that
restarts halfway through is worse than one that finishes.

**What is deliberately missing:** step 7, MCU standby with EXTI wake. The amp and the
flash are already below 1 µA between plays; what remains is the MCU's poll loop, a few
mA. It was left out rather than guessed at, because standby changes how the debug
interface behaves and a wrong first attempt looks exactly like a dead board.

### `src/audio.c` / `audio.h` — PWM output and double-buffered streaming

Two invariants hold this file together. Break either and the symptom is confusing.

**1. The sample clock is TIM2 — not TIM1, not SysTick.**

The carrier is 187.5 kHz and 187500 / 8000 = 23.4375, so no whole-number division of the
TIM1 update event lands on 8 kHz. A second timer off the same 48 MHz bus lands exactly:
48 000 000 / 8000 = 6000, so `ATRLR = 5999`.

It was SysTick first, for that same exact division, and that was a bug: ch32fun builds
`Delay_Us()`/`Delay_Ms()` on the free-running SysTick counter, so reprogramming SysTick
broke every delay in the firmware, and stopping it froze `CNT` so the next delay never
returned. See [`debug-log.md`](debug-log.md). TIM2 is otherwise unused and leaves
ch32fun's timebase alone.

`tick_start()` has one subtlety worth keeping: after `SWEVGR = TIM_UG` latches PSC and
ATRLR, `INTFR = 0` drops the update flag that UG itself just set — otherwise the ISR
fires immediately on enable.

**2. The ISR never touches SPI.** It reads from the active buffer, hands the spent one to
the main loop, and returns. A 256-byte page is 32 ms of audio and the SPI refill costs
well under a millisecond, so there is enormous slack — but only while the swap
discipline holds. *If you ever find yourself wanting to read flash from the ISR, that is
the bug.*

The buffer state machine, all `volatile`:

| Variable | Owner | Meaning |
|---|---|---|
| `buf[2][256]` | both | the audio |
| `fill[2]` | main writes, ISR reads | valid bytes; `0` = empty; `< 256` = last chunk of the chant |
| `play_idx`, `play_pos` | ISR | where playback is |
| `refill_idx` | ISR sets, main clears | `-1` = nothing pending |
| `running` | both | non-zero while playing |
| `played` | ISR | samples emitted since `audio_start()`; the diagnostic that says whether the ISR ran at all |

The ISR's end-of-buffer branch encodes two different endings. A **short** buffer
(`fill < 256`) is the tail of the chant: stop. A **full** buffer followed by an empty one
is either an exact page-boundary ending or a main-loop underrun — and that also stops,
rather than replaying stale bytes, because an underrun that loops old audio is far harder
to diagnose than one that goes quiet.

There is no lock between `audio_service()` and the ISR, and that is safe only because the
ISR is always at least a whole buffer (32 ms) ahead: exactly one buffer is ever pending
refill at a time.

`audio_start()` refuses anything that is not `FLASH_FMT_PCM8`, and refuses zero length or
zero rate. Refusing leaves `audio_busy()` at 0, which `main.c` notices and reports. ADPCM
would be decoded here — two samples per byte — and is not implemented.

`ramp_to()` is the anti-pop: 64 linear steps over `AUDIO_RAMP_MS`. Stepping from
mid-scale straight to the first sample is a step change of up to half the rail, and the
amp reproduces that faithfully as a click.

`pwm_init()` contains the classic CH32V003 gotcha: **TIM1 is an advanced-control timer,
so its outputs stay dead until `MOE` is set in `BDTR`.** "PWM looks configured, pin does
nothing" is almost always that one line missing.

### `src/spiflash.c` / `spiflash.h` — the FM25Q08B driver

Mode 0 (CPOL=0, CPHA=0), MSB first, **software** chip select. Nothing here is
FM25Q08B-specific except the JEDEC ID and the timeouts; any 25-series part would work.

Details that are load-bearing:

- `SSM | SSI` in `CTLR1`: with software NSS but without `SSI`, the peripheral believes it
  has been deselected and refuses to clock at all.
- CS is raised in the output register **before** the pin becomes an output, so it never
  spends a moment driven low. A stray low CS at power-on can leave the chip mid-command.
- MISO is an input **with the pull-up enabled**. The flash tri-states DO whenever CS is
  high; a floating input there drifts around the switching threshold and burns
  microamps, which matters on a primary cell.
- `spiflash_init()` reaches every SPI pin through `PIN_FLASH_CS_PORT`, which is correct
  only because CS/SCK/MOSI/MISO all live on GPIOC. If one ever moves ports, this file
  needs real per-pin port macros.
- `wait_ready()` polls SR1 BUSY every 100 µs. **Every program and erase must go through
  it** — a command issued while BUSY is set is silently ignored by the chip, which looks
  exactly like a bad solder joint.
- `spiflash_page_program()` refuses a write that would cross a 256-byte page boundary. A
  page program wraps around *inside* its own page rather than carrying into the next, so
  an unchecked write silently corrupts the start of the page it was in.
- `spiflash_unprotect()` clears the status-register block-protect bits. Parts usually
  ship unprotected, but a chip that has been in a programmer before may not be — and a
  protected chip accepts erase and program commands and quietly does nothing.
- `spiflash_wake()` waits 20 µs after `ABh`. tRES1 is a few microseconds; skipping the
  delay makes the first read after every wake come back as garbage.
- `spiflash_read_begin/next/end` is the streaming form: one `03h` command, then bytes for
  as long as CS stays low. The loader's CRC pass uses it; issuing a fresh command per
  byte would triple the time on the wire.

**The flash header** — written by the host, read by `spiflash_read_header()`:

```
0x00  "CHNT"      4 B
0x04  format      1 B    0 = 8-bit unsigned PCM, 1 = 4-bit IMA ADPCM (not implemented)
0x05  reserved    1 B
0x06  sample rate 4 B    little-endian, UNALIGNED
0x0A  data length 4 B    little-endian, UNALIGNED
0x0E  reserved    2 B
0x10  audio data ...
```

The two u32s sit at offsets 6 and 10, which are **not** 4-byte aligned, and the QingKe
V2A core does not do unaligned loads. Never cast the buffer to `uint32_t *` — the header
is assembled a byte at a time, on purpose.

Validation is deliberately narrow: length must be non-zero and fit in 1 MB minus the
header, and rate must be **2000–24000 Hz**. That range is the silent limit on how odd a
chant you can load.

---

## The loaders

### `src/loader/loader_swio.c` — the one in use

Same job as `loader.c`, same `spiflash.c` underneath, no UART. The host reaches it over
the WCH-LinkE's single SWIO wire, so a board with nothing on PD5/PD6 still loads a chant.

**A RAM mailbox, not a byte stream.** A debug probe can read and write target memory, so
the host halts the core, drops a 512-byte request into a fixed struct at `0x20000300`,
resumes, and the loader programs it out over SPI. There is no wire protocol, nothing to
resync, and no timeouts on either side. Commands: ping, JEDEC id, unprotect, sector
erase, program, CRC32 — the same set `loader.c` exposes as ASCII opcodes.

Three things in that file that look arbitrary and are not:

- **The mailbox address is a constant,** because the host must know it without parsing
  the ELF. `0x20000300` sits above this firmware's .data/.bss (256 B) and below the
  stack (from `0x20000800`). `main()` checks the lower boundary at boot and hangs
  deliberately if .bss ever grows into it — a host-side timeout with a clear message
  beats silent corruption.
- **`do_program()` copies out of the mailbox into `page[]` before programming.** The
  host can halt the core and overwrite `data[]` at any instant; programming from a
  private copy means the worst case is a stale page, which the CRC32 catches, rather
  than a page spliced from two chunks.
- **printf is compiled out** (`-DFUNCONF_USE_DEBUGPRINTF=0 -DFUNCONF_NULL_PRINTF=1` in
  both build systems). ch32fun's SWIO printf uses the debug module's data registers,
  which is exactly the channel this loader's host is driving. Two owners, one channel.

Why not ch32fun's SWIO printf channel as the transport, which looks ideal: it moves
three bytes per USB round trip, and on Windows minichlink reads the host end with
`_kbhit()`/`_getch()`, which only see a real console — no script can pipe into it.

### `src/loader/loader.c` — route C, the UART one

Flashed *instead of* the player. Receives the blob over UART and writes it into U3 page
by page. It is deliberately dumb: the host drives everything and the MCU executes one
command at a time and answers, so there is no state machine on this side to get out of
sync.

Protocol — one ASCII opcode, arguments after it, every reply starting with a status byte
(`K` ok, `E` flash timeout, `C` checksum or short read). Multi-byte integers on the wire
are **big-endian**, matching the order the flash chip wants addresses in. (The
little-endian fields inside the 16-byte header are *payload*, not protocol.)

| Command | Reply | Meaning |
|---|---|---|
| `?` | `K` `"CL01"` | ping and version; also how the host resyncs |
| `I` | `K` id2 id1 id0 | JEDEC ID |
| `U` | `K` \| `E` | clear block protect |
| `S` a2 a1 a0 | `K` \| `E` | erase one 4 KB sector |
| `P` a2 a1 a0 &lt;256 B&gt; sum | `K` \| `C` \| `E` | program one page |
| `X` a2 a1 a0 n3 n2 n1 n0 | `K` c3 c2 c1 c0 | CRC32 over a range |

Design points:

- **Every operand read is bounded at 50 ms** (`uart_rx_to()`), and that bound is the
  single most important robustness property of this firmware. Unbounded, a dropped byte
  parks the loader mid-command, where the host's `?` resync probes get consumed as
  operand bytes — and three of them complete an address and trigger a **real sector
  erase** that answers `K`. One corrupted byte turned into a storm of spurious erases the
  host could not tell from real ones. 50 ms sits deliberately between the two timescales
  that matter: far above the ~174 µs inter-byte gap of a back-to-back burst at 57600 (so a healthy
  transfer never trips it, and USB's 1 ms frame scheduling cannot either), and far below
  the host's 3 s status timeout. And because it only fires after 50 ms of silence, the
  host has demonstrably stopped sending, so no stray page bytes are left in flight to be
  misread as commands.
- The top-level `uart_rx()` is *not* bounded, and should not be: waiting for the next
  command is the loader's idle state.
- **Unknown bytes are ignored silently.** Line noise at connect time must not
  desynchronise anything; the host resyncs by sending `?` until it gets `K`.
- **`P` always carries exactly 256 bytes**, host-padded with `0xFF` (the erased state, so
  padding costs no wear). No length field to get wrong on either side. `sum` is the low
  byte of the sum of the three address bytes plus all 256 data bytes.
- **`X` is why verification is cheap.** Reading 1 MB back over the UART costs as long
  again as writing it; instead the MCU CRCs it and sends four bytes. Bitwise CRC-32/IEEE
  (reflected, poly `0xEDB88320`), the same polynomial as `zlib.crc32`, so the host
  compares directly. A table would be 4× faster and cost 1 KB of a 2 KB RAM budget — not
  worth it for a few seconds.
- **`amp_off()` first thing, never touched again.** A loader that made noise while it
  worked would also put a couple of hundred milliamps of class-D switching right next to
  the SPI bus.
- **`spiflash_wake()` at startup.** The player leaves the chip in deep power-down, and a
  sleeping chip ignores every command — which looks exactly like broken wiring.

---

## Host tools

### `tools/swio_link.py` — the SWIO transport

Two backends behind one interface, because which one can open the probe depends on the
machine, not on preference. `openocd` (the default) keeps one process up for the whole
load and drives it over its TCL port; `minichlink` spawns a process per chunk. On this
project's machine only OpenOCD works — see [`debug-log.md`](debug-log.md) #6.

The one piece of hard-won behaviour here: **on a fast backend the per-chunk
acknowledgement is required for correctness.** What matters between two chunks is not
elapsed time but how long the core is actually *running*, and nearly all of an OpenOCD
chunk is spent halted. `debug-log.md` #7 has the measurement.

### `tools/load_audio_swio.py` — the one you actually run

```sh
python tools/load_audio_swio.py
python tools/load_audio_swio.py other.raw --rate 8000
```

Flash `loader_swio` → push the audio over SWIO → CRC32 → flash the player back. ~40 s.
Step 3 runs even when step 2 fails, for the same reason as route C's version.

One structural detail worth knowing before editing it: **it opens the probe only
between the flashing steps, never across one.** PlatformIO launches its own OpenOCD to
upload, and two processes cannot own one WCH-Link — which is why `flash_firmware()`
takes `args` rather than a live link.

### `tools/load_audio.py` — route C's equivalent

```sh
python tools/load_audio.py --pio                            # chant8k-limit.raw, port auto-detected
python tools/load_audio.py --pio chant8k-limit.raw --port COM7
```

(`make load` is a thin wrapper over the same script, without `--pio`.)

Runs all three steps in order: flash the loader → `send_chant.py` → flash the player
back. Two deliberate behaviours:

- **Step 3 runs even when step 2 fails.** A board left holding the loader sits silent
  while you wonder why the button does nothing. `--keep-loader` opts out when you want to
  retry the transfer by hand.
- **The port is resolved before the MCU is touched.** Failing on port detection costs
  nothing; failing after step 1 leaves the loader on the board for no reason.

Auto-detection keeps any port with a USB VID and drops Bluetooth and other virtual ones,
which are always present on a laptop and produce a timeout indistinguishable from bad
wiring. On this board the right port is the **WCH-LinkE's own CDC interface** — it is a
debug probe *and* a USB-serial adapter — so the LinkE is a valid candidate, not something
to filter out. After flashing the loader, `minichlink -b` reboots the chip and that port
re-enumerates, so `wait_for_port()` waits for it to come back rather than racing it.

`--pio` switches the two flashing steps to the PlatformIO environments. Both backends
compile the same sources.

### `tools/send_chant.py` — the wire protocol, host side

Accepts a headerless `.raw` (and builds the 16-byte `CHNT` header) or a `.bin` that
already starts with `CHNT` (sent through unchanged). `--in-format u8|s8|s16` converts on
the way in; **unsigned 8-bit is what the firmware wants**, because the byte goes straight
into the PWM compare register where 128 is the idle mid-point. Signed samples play as
loud distortion.

Sequence: `sync()` → `I` JEDEC check (which fails loudly, with the `00 00 00` /
`FF FF FF` diagnosis) → `U` unprotect → erase every sector → resync → program every page
→ `X` CRC over the whole padded image, compared against `zlib.crc32`.

Two host-side details that were bugs and are now load-bearing:

- **`_read()` never assigns `self.ser.timeout`.** On Windows that setter reconfigures the
  already-open port, and reconfiguring mid-transaction discards bytes written but not yet
  clocked out — truncating the 261-byte page just sent, so the loader answers `C` to
  pages the host built perfectly. It is now a deadline loop over a port whose settings
  never change. **This was the bug that blocked the entire transfer.**
- **`_drain()` reads until the line has been silent**, rather than flushing once. The
  USB-serial bridge buffers, so a slow command can still be delivering replies long after
  the host gave up on it; only silence proves the loader has stopped talking. `sync()`
  sends **one** `?` per attempt for the same reason — a burst queues replies that
  desynchronise the next real command.

Erases and pages each get `--retries` (default 3) with a `sync()` between attempts, and
there is an explicit resync at the erase→program phase boundary: page 0 carries the
header the player looks for, and it is the worst place to discover the link is out of
step.

`--no-verify` skips the CRC. There is no good reason to.

### `tools/raw_resample.py` — the rate and loudness tool

Takes a headerless 8-bit unsigned `.raw` whose **real** rate you know and produces one at
the player's rate. Pipeline: one-pole high-pass (default 200 Hz) → windowed-sinc resample
(Blackman window, 97 taps, cutoff at 45 % of the *output* rate expressed against the
input rate) → unit scale → optional look-ahead limiter → optional tanh soft clip → peak
normalise to `--headroom` → back to unsigned 8-bit.

Why each stage exists:

- **Anti-alias low-pass before decimation.** Dropping every 5.5th sample folds everything
  above 4 kHz back down as a metallic buzz. This is the step `ffmpeg -ar` gets right and
  naive decimation gets wrong.
- **200 Hz high-pass.** The 28 mm driver starts near 700 Hz, so low content is headroom
  the chant never gets to use. Raising it *toward* 700 Hz measurably **costs** loudness on
  this recording (−14.1 → −15.0 dBFS at 400 Hz) — the peaks here are not LF-driven. Leave
  it at 200.
- **The limiter is where the loudness comes from.** Peak normalisation sets the peaks and
  says nothing about the average. The chant peaks at full scale but averages ~13.7 dB
  below it, so the amp idles through almost the whole thing — and there is no fix in the
  schematic, because R2/R3 already present ~0.32 Vpp against the PAM8302A's ~0.37 Vpp
  ceiling. The wasted headroom is *inside the file*. `--limit` closes the crest factor
  with the peaks still landing at `--headroom`, so **the amp sees the same maximum
  voltage and never clips harder**; the limiter only rescales the waveform and adds no
  distortion.
- **Look-ahead is not optional.** Without it the attack is a race the transient always
  wins: measured, it made the crest factor *worse* (13.7 → 14.9 dB). Two milliseconds of
  look-ahead turns that into 13.7 → 10.3 dB. The cost is 2 ms of latency, free in an
  offline tool.
- **`--soft-clip` bends the waveform**, so it buys the last few dB at a price. 1.0–1.5 is
  inaudible on a 28 mm driver, 2 gets edgy, 3+ is obviously crunchy.

Measured on this chant — and **`--limit 16` with no soft clip is the setting confirmed
right by ear on the hardware**:

| flags | crest factor |
|---|---|
| *(none)* | 13.7 dB |
| `--limit 16` | 10.3 dB |
| `--limit 16 --soft-clip 1.5` | 7.5 dB |

Limiter timing (attack 1 ms, release 60 ms, look-ahead 2 ms) is deliberately not exposed
on the command line: those are properties of speech, not of a particular chant.

### `tools/wav2raw.py` — a stub, and fine to leave alone

Never implemented. `ffmpeg -f u8 -ar 8000 -ac 1` plus `raw_resample.py` does everything it
was going to do, and `send_chant.py` builds the header. It is kept because its docstring
records the intended ADPCM encoder layout.

### `scripts/pio_send_chant.py`

Adds `pio run -e loader -t sendchant`, configured by the `custom_chant_*` options in
`platformio.ini`'s `[env:loader]`. It shells out to `tools/send_chant.py` using
PlatformIO's own interpreter, which already ships pyserial — that is the main reason it
shells out rather than importing. It deliberately ignores `upload_port`: that refers to
the debug probe's SWIO interface, not the serial adapter on PD5/PD6.

---

## Build systems — the one rule

**Two build systems, one set of sources.** `platformio.ini` is what this project is
flashed with (see [`hardware.md`](hardware.md#windows-the-driver-and-why-platformio-is-the-easier-route));
the Makefiles remain the reference build and compile the identical files. PlatformIO
compiles everything under `src/` by default and this tree now holds **three** `main()`s,
so each environment carries an explicit `build_src_filter` that starts with `-<*>` to
clear the default. The Makefile side is one file per image: `Makefile` (player),
`Makefile.loader` (UART), `Makefile.loader-swio` (SWIO).

> **Adding a `.c` file means editing two places**: the matching Makefile's
> `ADDITIONAL_C_FILES` and that environment's `build_src_filter`. Otherwise one build
> system silently links a different program than the other. The failure mode for getting
> the filter wrong is a duplicate-`main` link error that does not name the file it came
> from.

`-Isrc` in `build_flags` is required, not cosmetic: `loader.c` sits in a subdirectory and
includes `"board.h"` from the top of `src/`. A quote-include searches the including file's
own directory and then the include path — never the project's source root.

A `build_flags` inside an environment **replaces** the `[env]` one rather than adding to
it, which is why `[env:player]` has to repeat `${env.build_flags}`.

The PlatformIO platform (`Community-PIO-CH32V/platform-ch32v`) is fetched from git on the
first build and takes a few minutes once. `upload_protocol` is deliberately left unset:
the board default is normally right for a WCH-LinkE over SWIO, and a wrong value fails
louder than a missing one.

## Audio artifacts on disk

`.gitignore` excludes `*.raw` and `*.wav`, so these are not in git — worth knowing what
each one is before deleting the wrong one.

| File | Size | Really is | What it is |
|---|---|---|---|
| `NaKomNamoCompressed.raw` | 451 849 B | 10.25 s @ **44.1 kHz** | the original export. **The file that caused the wrong-rate bug** — loaded as 8 kHz it looks like a 56.5 s blob and plays 5.5× slow. |
| `NaKomNamo.raw` | 81 968 B | 10.25 s @ 8 kHz | an 8 kHz export of the same chant. Crest 13.7 dB — the "quiet" baseline all the loudness numbers are measured against. |
| `chant8k.raw` | 81 968 B | 10.25 s @ 8 kHz | `raw_resample.py` output without the limiter. Crest 14.0 dB. |
| `chant8k-limit.raw` | 81 968 B | 10.25 s @ 8 kHz | **the one that ships.** `--limit 16`, crest 10.3 dB. Referenced by `platformio.ini` and the `Makefile`'s `CHANT`. |
| `chant8k-limit.bin` | 81 984 B | — | the same bytes with the 16-byte `CHNT` header prepended — i.e. exactly what lands at flash address 0. |

10.25 s at 8 kHz is 82 KB, about **8 % of the 1 MB chip**. Capacity was never remotely a
constraint for this chant, which is why ADPCM was never needed.
