#!/usr/bin/env python3
"""
send_chant.py - push the chant into U3 through the loader firmware.

Flash src/loader/loader.c first (make -f Makefile.loader flash), wire a
USB-serial adapter to PD5/PD6/GND, then:

    python tools/send_chant.py COM7 chant.raw
    python tools/send_chant.py /dev/ttyUSB0 chant.raw

Input may be either:

  * a raw headerless blob - 8-bit UNSIGNED mono PCM at 8 kHz by default. The
    16-byte "CHNT" header is built here and written at address 0.
  * a .bin that already starts with "CHNT" - sent through unchanged.

Make a raw blob from any recording with ffmpeg:

    ffmpeg -i chant.wav -ac 1 -ar 8000 -f u8 -acodec pcm_u8 chant.raw

Unsigned 8-bit is not an arbitrary choice: the byte goes straight into the PWM
compare register, where 128 is the idle mid-point. Signed samples would play as
loud distortion.

Needs pyserial:  pip install pyserial
"""

import argparse
import struct
import sys
import time
import zlib

try:
    import serial
except ImportError:
    sys.exit("pyserial is not installed:  pip install pyserial")

# ---- must stay in sync with src/spiflash.h --------------------------------

MAGIC = b"CHNT"
HDR_SIZE = 16
DATA_ADDR = 0x000010
PAGE = 256
SECTOR = 4096
FLASH_SIZE = 1024 * 1024
FMT_PCM8 = 0
FMT_ADPCM4 = 1

JEDEC_EXPECTED = bytes((0xA1, 0x40, 0x14))


def build_header(fmt: int, rate: int, length: int) -> bytes:
    """4 B magic | fmt u8 | pad u8 | rate u32 LE | length u32 LE | pad to 16."""
    hdr = MAGIC + struct.pack("<BBII", fmt, 0, rate, length)
    return hdr.ljust(HDR_SIZE, b"\x00")


# ---- link -----------------------------------------------------------------


class LoaderError(RuntimeError):
    pass


class Loader:
    def __init__(self, port: str, baud: int, verbose: bool = False):
        # A short read timeout is fine for everything except erase and CRC,
        # which pass their own.
        # A short, FIXED read timeout. _read() layers its own deadline on top,
        # so this only controls how often that loop wakes up - and because it is
        # never reassigned, the port is never reconfigured mid-transaction.
        self.ser = serial.Serial(port, baud, timeout=0.05, write_timeout=10.0)
        self.verbose = verbose

    def close(self):
        self.ser.close()

    def _read(self, n: int, timeout: float) -> bytes:
        """Read exactly n bytes, giving up after `timeout` seconds.

        Deliberately does NOT assign self.ser.timeout. On Windows that setter
        reconfigures the already-open port, and a reconfigure in the middle of a
        transaction can discard bytes that have been written but not yet clocked
        out - truncating the 261-byte page we just sent, so the loader answers
        'C' for a page the host built perfectly. A deadline loop over a port
        whose settings never change avoids the whole problem.
        """
        deadline = time.monotonic() + timeout
        data = bytearray()
        while len(data) < n:
            chunk = self.ser.read(n - len(data))
            if chunk:
                data += chunk
                continue
            if time.monotonic() >= deadline:
                break
        if len(data) != n:
            raise LoaderError(f"timed out waiting for {n} byte(s), got {len(data)}")
        return bytes(data)

    def _status(self, timeout: float) -> str:
        s = self._read(1, timeout).decode("latin-1")
        if s == "C":
            raise LoaderError("checksum mismatch reported by the loader")
        if s == "E":
            raise LoaderError("flash operation timed out on the MCU")
        if s != "K":
            raise LoaderError(f"unexpected status byte {s!r}")
        return s

    def _drain(self, quiet: float = 0.08, limit: float = 2.0) -> bytes:
        """Read until the line has been silent for `quiet` seconds.

        The USB-serial bridge buffers host->MCU bytes, so a command that was
        slow to answer can still be delivering replies long after we gave up on
        it. A single flush cannot see those - they have not arrived yet. Only
        silence proves the loader has finished talking.
        """
        seen = bytearray()
        deadline = time.time() + limit
        while time.time() < deadline:
            n = self.ser.in_waiting
            if n:
                seen += self.ser.read(n)
            else:
                time.sleep(quiet)
                if not self.ser.in_waiting:
                    break
        return bytes(seen)

    def sync(self, tries: int = 10) -> str:
        """Re-establish the '?' -> 'K' handshake from an unknown state.

        One probe per attempt, never a burst. Sending '?' repeatedly without
        waiting queues them in the bridge; each one is answered with five bytes
        and the backlog is what desynchronises the next real command.
        """
        for _ in range(tries):
            self._drain()
            self.ser.write(b"?")
            self.ser.flush()
            try:
                self._status(0.5)
                version = self._read(4, 0.5).decode("latin-1")
            except LoaderError:
                continue
            # The handshake is only trustworthy if nothing follows it.
            if self._drain():
                continue
            return version
        raise LoaderError(
            "no reply from the loader.\n"
            "  - is loader.c flashed (not the player)?\n"
            "  - adapter RX to PD5 and adapter TX to PD6? they cross\n"
            "  - GND shared between the adapter and the board?"
        )

    def jedec_id(self) -> bytes:
        self.ser.write(b"I")
        self._status(2.0)
        return self._read(3, 2.0)

    def unprotect(self):
        self.ser.write(b"U")
        self._status(2.0)

    def erase_sector(self, addr: int):
        self.ser.write(b"S" + addr.to_bytes(3, "big"))
        self._status(3.0)

    def program_page(self, addr: int, data: bytes):
        assert len(data) == PAGE
        a = addr.to_bytes(3, "big")
        checksum = (sum(a) + sum(data)) & 0xFF
        self.ser.write(b"P" + a + data + bytes((checksum,)))
        self.ser.flush()
        self._status(3.0)

    def crc32(self, addr: int, length: int, timeout: float = 120.0) -> int:
        self.ser.write(b"X" + addr.to_bytes(3, "big") + length.to_bytes(4, "big"))
        self._status(timeout)
        return int.from_bytes(self._read(4, timeout), "big")


# ---- input handling -------------------------------------------------------


def load_blob(path: str, rate: int, in_format: str) -> bytes:
    raw = open(path, "rb").read()
    if not raw:
        sys.exit(f"{path} is empty")

    if raw[:4] == MAGIC:
        if in_format != "u8":
            print("note: input already has a CHNT header; --in-format ignored")
        return raw

    if in_format == "s16":
        if len(raw) % 2:
            sys.exit("s16 input has an odd byte count - not 16-bit samples?")
        # Signed 16-bit LE -> unsigned 8-bit: take the high byte and flip the
        # sign bit. Exactly the mapping the PWM wants, and it is what ffmpeg's
        # pcm_u8 encoder does.
        raw = bytes((raw[i + 1] ^ 0x80) for i in range(0, len(raw), 2))
    elif in_format == "s8":
        raw = bytes((b ^ 0x80) for b in raw)
    elif in_format != "u8":
        sys.exit(f"unknown --in-format {in_format}")

    return build_header(FMT_PCM8, rate, len(raw)) + raw


def describe(blob: bytes):
    fmt, _, rate, length = struct.unpack("<BBII", blob[4:14])
    name = {FMT_PCM8: "8-bit PCM", FMT_ADPCM4: "4-bit IMA ADPCM"}.get(fmt, f"fmt {fmt}")
    secs = length / rate * (2 if fmt == FMT_ADPCM4 else 1)
    print(f"  format   {name}")
    print(f"  rate     {rate} Hz")
    print(f"  audio    {length} bytes, ~{secs:.1f} s")
    print(f"  total    {len(blob)} bytes ({len(blob) / FLASH_SIZE * 100:.1f} % of the chip)")
    if fmt == FMT_ADPCM4:
        print("  WARNING: the player firmware only decodes 8-bit PCM today.")


# ---- main -----------------------------------------------------------------


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("port", help="serial port, e.g. COM7 or /dev/ttyUSB0")
    ap.add_argument("input", help="chant.raw (headerless) or chant.bin (has CHNT)")
    # Must match LOADER_BAUD in src/board.h. 57600, not 115200: at the faster
    # rate the hand-wired link corrupted bytes often enough to derail the
    # transfer. Change both together or the loader will not answer at all.
    ap.add_argument("--baud", type=int, default=57600)
    ap.add_argument("--rate", type=int, default=8000, help="sample rate of a raw input")
    ap.add_argument(
        "--in-format",
        default="u8",
        choices=("u8", "s8", "s16"),
        help="sample format of a raw input (default u8, what the firmware wants)",
    )
    ap.add_argument("--no-verify", action="store_true", help="skip the CRC32 check")
    ap.add_argument("--retries", type=int, default=3, help="retries per page")
    args = ap.parse_args()

    blob = load_blob(args.input, args.rate, args.in_format)
    print(f"{args.input}:")
    describe(blob)
    if len(blob) > FLASH_SIZE:
        sys.exit(f"too big: {len(blob)} B > {FLASH_SIZE} B - shorten the chant")

    # Pad to a whole page with 0xFF, the erased state, so the last page program
    # writes nothing it does not have to.
    padded = blob + b"\xff" * (-len(blob) % PAGE)
    n_pages = len(padded) // PAGE
    n_sectors = (len(padded) + SECTOR - 1) // SECTOR

    print(f"\nopening {args.port} at {args.baud} 8N1")
    link = Loader(args.port, args.baud)
    try:
        version = link.sync()
        print(f"loader {version} responding")

        jid = link.jedec_id()
        pretty = " ".join(f"{b:02X}" for b in jid)
        if jid != JEDEC_EXPECTED:
            expected = " ".join(f"{b:02X}" for b in JEDEC_EXPECTED)
            sys.exit(
                f"JEDEC ID reads {pretty}, expected {expected} (FM25Q08B).\n"
                "  00 00 00 -> MISO (PC7) or the chip's 3 V is wrong\n"
                "  FF FF FF -> CS# (PC4) never gets pulled low\n"
                "Fix the wiring before writing anything."
            )
        print(f"flash JEDEC ID {pretty} - FM25Q08B confirmed")

        link.unprotect()

        t0 = time.time()
        print(f"erasing {n_sectors} sector(s) of 4 KB")
        for i in range(n_sectors):
            # Same retry/resync as the page loop below. A corrupted status byte
            # on a long perfboard UART run is not rare, and an erase that is
            # retried costs a few ms - while an unretried one throws away the
            # whole transfer. sync() drains the pipe and re-establishes the
            # 'K' handshake, so a late or garbled reply cannot desynchronise
            # the next command.
            for attempt in range(args.retries):
                try:
                    link.erase_sector(i * SECTOR)
                    break
                except LoaderError as exc:
                    if attempt == args.retries - 1:
                        raise LoaderError(f"sector {i} at 0x{i * SECTOR:06X}: {exc}")
                    link.sync()
            if i % 8 == 0 or i == n_sectors - 1:
                pct = (i + 1) / n_sectors * 100
                print(f"\r  {i + 1}/{n_sectors}  {pct:5.1f} %", end="", flush=True)
        print()

        # Resynchronise at the phase boundary. An erase that needed a retry can
        # leave bytes queued in the USB-serial bridge heading for the MCU; the
        # host cannot see that direction, so "the line is quiet" is not proof
        # the loader is idle. One clean '?'/'K' round trip before the first page
        # is - and page 0 is the worst place to discover otherwise, since it
        # carries the CHNT header the player looks for.
        link.sync()

        print(f"writing {n_pages} page(s) of 256 B")
        for i in range(n_pages):
            addr = i * PAGE
            chunk = padded[addr : addr + PAGE]
            for attempt in range(args.retries):
                try:
                    link.program_page(addr, chunk)
                    break
                except LoaderError as exc:
                    if attempt == args.retries - 1:
                        raise LoaderError(f"page {i} at 0x{addr:06X}: {exc}")
                    link.sync()
            if i % 32 == 0 or i == n_pages - 1:
                pct = (i + 1) / n_pages * 100
                elapsed = time.time() - t0
                print(f"\r  {i + 1}/{n_pages}  {pct:5.1f} %  {elapsed:5.1f} s",
                      end="", flush=True)
        print()

        if args.no_verify:
            print("skipping verification (--no-verify)")
        else:
            print("verifying (CRC32 computed on the MCU, over the whole image)")
            want = zlib.crc32(padded) & 0xFFFFFFFF
            got = link.crc32(0, len(padded))
            if got != want:
                sys.exit(f"CRC MISMATCH: flash {got:#010x}, file {want:#010x}\n"
                         "The load is not trustworthy - run it again.")
            print(f"  CRC32 {got:#010x} - matches")

        print(f"\ndone in {time.time() - t0:.1f} s.")
        print("Now flash the player:  make flash")
    except LoaderError as exc:
        print(f"\nERROR: {exc}", file=sys.stderr)
        return 1
    finally:
        link.close()

    return 0


if __name__ == "__main__":
    sys.exit(main())
