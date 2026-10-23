#!/usr/bin/env python3
"""
load_audio_swio.py - load the chant into U3 over SWIO alone. One command.

    python tools/load_audio_swio.py                       chant8k-limit.raw
    python tools/load_audio_swio.py other.raw --rate 8000

This is route D in docs/audio-pipeline.md. The WCH-LinkE's single SWIO wire
(PD1) is the only connection to the board: no USB-serial adapter, nothing on
PD5/PD6. Same three steps as tools/load_audio.py, same end result, different
transport:

  1. flash src/loader/loader_swio.c to the MCU
  2. push the audio into U3 through the loader's RAM mailbox, over SWIO
  3. flash the player back to the MCU

Step 3 runs even when step 2 fails, so the board is never left holding the
loader. --keep-loader opts out when you want to retry a transfer by hand.

Speed depends on the backend tools/swio_link.py picks. OpenOCD, the
default, keeps one process up for the whole transfer, so the 82 KB chant takes
seconds. The minichlink fallback spends ~250 ms per chunk re-opening USB and
takes about a minute. Either way the SPI writing itself is milliseconds.

Needs no pyserial and no serial port.
"""

import argparse
import os
import struct
import subprocess
import sys
import time
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

from swio_link import (  # noqa: E402
    JEDEC_EXPECTED,
    LOADER_VERSION,
    MBOX_DATA_MAX,
    SwioError,
    SwioLink,
)

# ---- must stay in sync with src/spiflash.h and tools/send_chant.py --------

MAGIC = b"CHNT"
HDR_SIZE = 16
PAGE = 256
SECTOR = 4096
FLASH_SIZE = 1024 * 1024
FMT_PCM8 = 0


def build_header(fmt: int, rate: int, length: int) -> bytes:
    """4 B magic | fmt u8 | pad u8 | rate u32 LE | length u32 LE | pad to 16."""
    hdr = MAGIC + struct.pack("<BBII", fmt, 0, rate, length)
    return hdr.ljust(HDR_SIZE, b"\x00")


def build_image(path: str, rate: int) -> bytes:
    """The exact bytes that belong at flash address 0."""
    with open(path, "rb") as f:
        blob = f.read()

    if blob[:4] == MAGIC:
        # Already a .bin with a header - the rate lives inside it, and ours
        # would be a guess. send_chant.py behaves the same way.
        print(f"{os.path.basename(path)} already has a CHNT header; --rate ignored")
        return blob

    if not blob:
        raise SystemExit(f"{path} is empty")

    return build_header(FMT_PCM8, rate, len(blob)) + blob


# ---- firmware -------------------------------------------------------------

def run(cmd) -> int:
    print("+ " + " ".join(cmd), flush=True)
    return subprocess.call(cmd, cwd=PROJECT)


def flash_firmware(env: str, args, make_target: str) -> None:
    """Put one of the two firmwares on the MCU.

    Nothing may hold the probe while this runs. PlatformIO launches its own
    OpenOCD to upload, and two processes cannot own one WCH-Link - so the
    caller opens its SwioLink only *between* the flashing steps, never across
    one. That is also why this takes `args` rather than a live link.

    PlatformIO is the default because docs/hardware.md records that `make
    flash` cannot bind WinUSB on this machine. --direct skips the build system
    and writes an image that is already built, which is quicker when you are
    only reloading audio and the firmware has not changed.
    """
    if not args.direct:
        if run(["pio", "run", "-t", "upload", "-e", env]) != 0:
            raise SwioError(f"flashing the {env} firmware failed")
        return

    # Prefer the ELF PlatformIO last built: it carries its own load addresses,
    # so the backend need not know where this chip's flash is mapped. Fall
    # back to the Makefile's .bin at the project root.
    candidates = [
        os.path.join(PROJECT, ".pio", "build", env, "firmware.elf"),
        os.path.join(PROJECT, make_target),
    ]
    for image in candidates:
        if os.path.isfile(image):
            with SwioLink(args.backend, args.tool_path, args.verbose) as link:
                link.flash_firmware(image)
            return

    raise SwioError(
        "no prebuilt image to flash. Looked for:\n  " + "\n  ".join(candidates) +
        f"\nBuild it first (`pio run -e {env}`) or drop --direct."
    )


# ---- the transfer ---------------------------------------------------------

def erase_range(link: SwioLink, nbytes: int) -> None:
    sectors = (nbytes + SECTOR - 1) // SECTOR
    for i in range(sectors):
        link.erase_sector(i * SECTOR)
        print(f"\r  erase  {i + 1}/{sectors} sectors", end="", flush=True)
    print()


def program_image(link: SwioLink, image: bytes, verify_each: bool) -> None:
    """Write the image, then make the MCU prove it.

    `verify_each` reads an acknowledgement back after every chunk. On the
    OpenOCD backend it is MANDATORY, not an option, and the reason is not
    obvious: what matters between two chunks is not wall-clock time but how
    long the core is actually *running*, and almost all of the gap is spent
    halted. OpenOCD halts, writes the scratch file, loads it, resumes - and
    the only part the loader gets to execute in is the short stretch between
    that resume and the next halt. Measured, that is a few milliseconds,
    against up to 6 ms for the two page programs a 512-byte chunk costs. So
    the loader is still mid-write when the next chunk overwrites its mailbox.

    This is not theoretical. Skipping the ack here produced a clean run that
    failed the final CRC32 (MCU 0x9f1058c5 against host 0xce244443) with no
    other symptom. Do not "optimise" it back.

    The minichlink backend is safe without it for the opposite reason: the
    core runs for the whole ~250 ms of the next process's startup and USB
    enumeration, which is forty times the margin needed.
    """
    # Pad to a whole page with 0xFF, the erased state, so the last write costs
    # nothing extra and the CRC the MCU computes covers a defined range.
    padded = image + b"\xff" * (-len(image) % PAGE)
    chunks = (len(padded) + MBOX_DATA_MAX - 1) // MBOX_DATA_MAX
    started = time.time()

    for i in range(chunks):
        off = i * MBOX_DATA_MAX
        link.program(off, padded[off:off + MBOX_DATA_MAX], wait=verify_each)
        done = off + MBOX_DATA_MAX
        rate = done / max(time.time() - started, 0.001)
        print(f"\r  write  {i + 1}/{chunks} chunks  "
              f"{min(done, len(padded))}/{len(padded)} B  "
              f"{rate / 1024:.1f} kB/s", end="", flush=True)
    print()

    expect = zlib.crc32(padded) & 0xFFFFFFFF
    print("  verify CRC32 over the whole image ...", end="", flush=True)
    got = link.crc32(0, len(padded))
    if got != expect:
        raise SwioError(
            f"CRC32 mismatch: MCU says {got:#010x}, host says {expect:#010x}.\n"
            "Something was written wrong. Run again with --verify-each to find "
            "the chunk that failed."
        )
    print(f" {got:#010x} ok")


# ---- main -----------------------------------------------------------------

def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("input", nargs="?", default="chant8k-limit.raw",
                    help="chant .raw or .bin (default: chant8k-limit.raw)")
    ap.add_argument("--rate", type=int, default=8000,
                    help="sample rate stamped into the header (default 8000)")
    ap.add_argument("--backend", default="auto",
                    choices=["auto", "openocd", "minichlink"],
                    help="which debug tool drives the probe (default: auto, "
                         "which prefers OpenOCD)")
    ap.add_argument("--direct", action="store_true",
                    help="flash prebuilt .bin images through the backend "
                         "instead of running PlatformIO")
    ap.add_argument("--keep-loader", action="store_true",
                    help="do not put the player back at the end")
    ap.add_argument("--verify-each", action="store_true",
                    help="read back an ack after every chunk. Roughly "
                         "doubles the time; the CRC32 at the end already "
                         "proves the whole image, so use this only to "
                         "localise a failure")
    ap.add_argument("--tool-path", default="",
                    help="full path to the backend tool to use")
    ap.add_argument("-v", "--verbose", action="store_true",
                    help="echo every command sent to the probe")
    args = ap.parse_args()

    audio = args.input if os.path.isabs(args.input) \
        else os.path.join(PROJECT, args.input)
    if not os.path.isfile(audio):
        sys.exit(f"no audio file at {audio}\n"
                 "  ffmpeg -i chant.wav -ac 1 -ar 8000 -f u8 -acodec pcm_u8 chant.raw")

    image = build_image(audio, args.rate)
    if len(image) > FLASH_SIZE:
        sys.exit(f"{len(image)} B does not fit in the 1 MB flash")

    secs = (len(image) - HDR_SIZE) / float(args.rate)
    print(f"{os.path.basename(audio)}: {len(image)} B "
          f"({len(image) - HDR_SIZE} B of audio, ~{secs:.1f} s at {args.rate} Hz)")

    rc = 0

    try:
        print("\n=== 1/3  loader_swio -> MCU ===")
        flash_firmware("loader_swio", args, "src/loader/loader_swio.bin")

        # Only now open the probe for ourselves - see flash_firmware().
        print("\n=== 2/3  audio -> U3 over SWIO ===")
        with SwioLink(args.backend, args.tool_path, args.verbose) as link:
            print(f"  backend {link.backend}")
            link.wait_for_loader()

            ver = link.ping()
            if ver != LOADER_VERSION:
                raise SwioError(
                    f"loader answered version {ver:#010x}, expected "
                    f"{LOADER_VERSION:#010x}. Is an old loader_swio on the MCU?"
                )

            jid = link.jedec_id()
            if jid != JEDEC_EXPECTED:
                raise SwioError(
                    f"flash JEDEC id is {jid:#08x}, expected "
                    f"{JEDEC_EXPECTED:#08x}. 0x000000 means MISO or power is "
                    "wrong; 0xffffff usually means CS# never went low. Fix "
                    "that before anything else - see docs/bring-up.md stage 5."
                )
            print(f"  flash  JEDEC {jid:#08x} ok")

            link.unprotect()
            erase_range(link, len(image))
            # link.fast is not a preference, it is a correctness requirement.
            # See program_image().
            program_image(link, image, args.verify_each or link.fast)
            print(f"  {link.calls} {link.backend} operations")

    except SwioError as e:
        print(f"\nerror: {e}", file=sys.stderr)
        rc = 1
    except KeyboardInterrupt:
        print("\ninterrupted", file=sys.stderr)
        rc = 130

    # Always, even after a failure: a board holding the loader sits silent
    # while you wonder why the button does nothing.
    if not args.keep_loader:
        print("\n=== 3/3  player -> MCU ===")
        try:
            flash_firmware("player", args, "chant.bin")
        except SwioError as e:
            print(f"error: {e}", file=sys.stderr)
            return 1
    else:
        print("\n--keep-loader: loader_swio is still on the MCU.")

    if rc == 0:
        print("\ndone. Press the switch.")
    return rc


if __name__ == "__main__":
    sys.exit(main())
