#!/usr/bin/env python3
"""
load_audio.py - run the whole three-step audio load as one command.

    python tools/load_audio.py --pio chant8k-limit.raw

Steps, in order:

  1. flash src/loader/loader.c to the MCU  (WCH-LinkE, SWIO)
  2. push the audio into U3 over UART      (tools/send_chant.py, PD5/PD6)
  3. flash the player back to the MCU      (WCH-LinkE, SWIO)

Step 3 runs even when step 2 fails. A board left holding the loader is a board
that sits there silent while you wonder why the button does nothing, so the
default is always to put the product firmware back. --keep-loader opts out when
you want to retry the transfer by hand.

The serial port is whatever is wired to PD5/PD6. On this board that is the
WCH-LinkE's own serial interface - it exposes a debug interface AND a CDC port -
so the LinkE is a valid candidate, not something to filter out. With no --port
the port is auto-detected; virtual ports with no USB identity, Bluetooth
especially, are ignored.

Build backend is the Makefiles by default; --pio uses the PlatformIO
environments instead. Both compile the same sources. On Windows, where
minichlink needs a WinUSB binding the Makefile route does not set up,
--pio is the one that works - see docs/hardware.md.

Needs pyserial (already required by send_chant.py):  pip install pyserial
"""

import argparse
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.dirname(HERE)

WCH_VID = 0x1A86


def looks_like_a_real_port(p) -> bool:
    """A physical USB serial device, as opposed to a Bluetooth or virtual one.

    Bluetooth RFCOMM ports enumerate with no USB VID and are always present on
    a laptop; picking one produces a timeout indistinguishable from bad wiring.
    """
    if not p.vid:
        return False
    return "bluetooth" not in (p.description or "").lower()


def run(cmd, cwd=PROJECT) -> int:
    print("+ " + " ".join(cmd), flush=True)
    return subprocess.call(cmd, cwd=cwd)


def build_cmds(use_pio):
    """(flash-loader, flash-player) commands for the chosen backend."""
    if use_pio:
        pio = ["pio", "run", "-t", "upload", "-e"]
        return pio + ["loader"], pio + ["player"]
    make = os.environ.get("MAKE", "make")
    return [make, "-f", "Makefile.loader", "flash"], [make, "flash"]


def autodetect_port() -> str:
    try:
        from serial.tools import list_ports
    except ImportError:
        sys.exit("pyserial is not installed:  pip install pyserial")

    ports = list(list_ports.comports())
    candidates = [p for p in ports if looks_like_a_real_port(p)]
    listing = "\n".join(f"  {p.device}  {p.description}" for p in ports) or "  none"

    if len(candidates) == 1:
        p = candidates[0]
        note = "  (WCH-LinkE serial interface)" if p.vid == WCH_VID else ""
        print(f"auto-detected serial port {p.device} ({p.description}){note}")
        return p.device
    if not candidates:
        sys.exit("no USB serial port found - only virtual/Bluetooth ones.\n"
                 "Is the WCH-LinkE (or a USB-serial dongle) plugged in?\n"
                 "Ports seen:\n" + listing)
    sys.exit("several serial ports present - say which one with --port:\n" + listing)


def wait_for_port(port: str, settle: float, extra: float = 6.0):
    """Block until `port` is enumerated again, then pause `settle` seconds."""
    from serial.tools import list_ports

    deadline = time.time() + extra
    while time.time() < deadline:
        if any(p.device == port for p in list_ports.comports()):
            break
        time.sleep(0.2)
    time.sleep(settle)


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("input", nargs="?", default="chant8k-limit.raw",
                    help="chant .raw or .bin (default: chant8k-limit.raw)")
    ap.add_argument("--port", help="serial port on PD5/PD6 (default: auto-detect)")
    ap.add_argument("--rate", type=int, default=8000,
                    help="sample rate stamped into the header (default 8000)")
    ap.add_argument("--pio", action="store_true",
                    help="build/flash with PlatformIO instead of the Makefiles")
    ap.add_argument("--keep-loader", action="store_true",
                    help="do not reflash the player at the end")
    ap.add_argument("--settle", type=float, default=2.0,
                    help="seconds to wait after the loader boots (default 2.0)")
    ap.add_argument("extra", nargs=argparse.REMAINDER,
                    help="-- followed by extra send_chant.py flags")
    args = ap.parse_args()

    audio = args.input if os.path.isabs(args.input) else os.path.join(PROJECT, args.input)
    if not os.path.isfile(audio):
        sys.exit(f"no audio file at {audio}\n"
                 "  ffmpeg -i chant.wav -ac 1 -ar 8000 -f u8 -acodec pcm_u8 chant.raw")

    # Resolve the port before touching the MCU. Failing here costs nothing;
    # failing after step 1 leaves the loader on the board for no reason.
    port = args.port or os.environ.get("CHANT_PORT") or autodetect_port()

    flash_loader, flash_player = build_cmds(args.pio)
    extra = [a for a in args.extra if a != "--"]

    print("\n=== 1/3  loader -> MCU ===")
    if run(flash_loader) != 0:
        sys.exit("flashing the loader failed - nothing was written to the flash.")

    # minichlink's -b reboots the chip, and here the serial port lives on the
    # same LinkE that was just driven, so it can re-enumerate. Wait for the port
    # to come back rather than racing it: send_chant would otherwise die on
    # "could not open port" instead of on anything diagnostic.
    wait_for_port(port, args.settle)

    print("\n=== 2/3  audio -> SPI flash ===")
    send = [sys.executable, os.path.join(HERE, "send_chant.py"),
            port, audio, "--rate", str(args.rate)] + extra
    rc = run(send)

    if args.keep_loader:
        print("\n--keep-loader: the loader is still on the MCU.")
        return rc

    print("\n=== 3/3  player -> MCU ===")
    back = "pio run -e player -t upload" if args.pio else "make flash"
    if run(flash_player) != 0:
        sys.exit("flashing the player failed - the LOADER is still on the MCU.\n"
                 f"Run `{back}` before expecting the button to work.")

    if rc != 0:
        print("\nThe player is back on the MCU, but the transfer above FAILED -\n"
              "the flash contents are not what you asked for.")
        return rc

    print("\nDone. Press SW1.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
