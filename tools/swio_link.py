#!/usr/bin/env python3
"""
swio_link.py - talk to src/loader/loader_swio.c over the WCH-LinkE's SWIO wire.

No UART, no second cable: the one pin that programs the MCU also carries the
chant. This module is the host half of the RAM mailbox described at the top of
src/loader/loader_swio.c; tools/load_audio_swio.py is what you actually run.

How it works
------------
A debug probe can read and write target memory, halting the core for the
transfer and resuming it afterwards. So one request is: halt, drop 512 bytes
into the loader's mailbox in RAM, resume - and the loader programs them out
over SPI. No wire protocol, nothing to resync.

Two backends do that, because which one can open the probe depends on the
machine:

  openocd     (default) the OpenOCD that PlatformIO installs. ONE process
              stays up for the whole transfer and takes commands over its TCL
              port, so a chunk costs milliseconds.
  minichlink  one process per chunk, ~250 ms each of USB re-enumeration.
              Portable, and the only option where OpenOCD is not installed.

On this machine OpenOCD is the one that works: the WCH-LinkRV interface is
bound to WCH's own driver, which OpenOCD and wlink use, while minichlink wants
WinUSB and fails with libusb_open() = -5. `--backend minichlink` is there for
the other case. See docs/hardware.md.

Why not ch32fun's SWIO printf channel, which looks like the obvious transport:
it moves three bytes per USB round trip, and on Windows minichlink reads the
host end of it with _kbhit()/_getch(), which only see a real console - so no
script can pipe into it.
"""

import os
import re
import socket
import struct
import subprocess
import sys
import tempfile
import time

# ---- must stay in sync with src/loader/loader_swio.c ----------------------

MBOX_ADDR = 0x20000300
MBOX_MAGIC = 0x434C5357          # "CLSW"
MBOX_HDR_SIZE = 32
MBOX_DATA_MAX = 512
LOADER_VERSION = 0x53570100

CMD_PING = 1
CMD_JEDEC = 2
CMD_UNPROTECT = 3
CMD_ERASE = 4
CMD_PROGRAM = 5
CMD_CRC32 = 6

ST_OK = 0
ST_NAMES = {
    0: "ok",
    1: "flash timed out (chip did not clear BUSY)",
    2: "bad arguments",
    3: "unknown command",
}

JEDEC_EXPECTED = 0x00A14014

PIO_PACKAGES = os.path.join(os.path.expanduser("~"), ".platformio", "packages")
HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.dirname(HERE)
EXE = ".exe" if os.name == "nt" else ""


class SwioError(RuntimeError):
    pass


# ---- finding the tools ----------------------------------------------------

def find_minichlink(explicit: str = "") -> str:
    if explicit:
        if not os.path.isfile(explicit):
            raise SwioError(f"no minichlink at {explicit}")
        return explicit

    env = os.environ.get("CHANT_MINICHLINK")
    if env:
        if not os.path.isfile(env):
            raise SwioError(f"CHANT_MINICHLINK points at nothing: {env}")
        return env

    candidates = [
        os.path.join(PIO_PACKAGES, "tool-minichlink", "minichlink" + EXE),
        os.path.join(PIO_PACKAGES, "framework-ch32v003fun", "minichlink",
                     "minichlink" + EXE),
        os.path.join(PROJECT, "ch32v003fun", "minichlink", "minichlink" + EXE),
    ]
    for c in candidates:
        if os.path.isfile(c):
            return c

    from shutil import which
    found = which("minichlink")
    if found:
        return found

    raise SwioError("could not find minichlink. Looked in:\n  " +
                    "\n  ".join(candidates) +
                    "\nSet CHANT_MINICHLINK to its full path.")


def find_openocd(explicit: str = "") -> tuple:
    """Return (openocd binary, directory holding wch-riscv.cfg)."""
    if explicit:
        if not os.path.isfile(explicit):
            raise SwioError(f"no openocd at {explicit}")
        return explicit, os.path.dirname(explicit)

    env = os.environ.get("CHANT_OPENOCD")
    if env:
        if not os.path.isfile(env):
            raise SwioError(f"CHANT_OPENOCD points at nothing: {env}")
        return env, os.path.dirname(env)

    base = os.path.join(PIO_PACKAGES, "tool-openocd-riscv-wch", "bin")
    exe = os.path.join(base, "openocd" + EXE)
    if os.path.isfile(exe) and os.path.isfile(os.path.join(base, "wch-riscv.cfg")):
        return exe, base

    raise SwioError(
        "could not find PlatformIO's OpenOCD for WCH.\n"
        f"Expected {exe}.\n"
        "Run `pio run -e player -t upload` once to install it, point "
        "CHANT_OPENOCD at one, or use --backend minichlink."
    )


# ---- transports -----------------------------------------------------------

class _Transport:
    """halt, poke memory, resume. Everything above this is backend-agnostic."""

    fast = False        # can we afford to read an ack after every chunk?
    name = "?"

    def close(self):
        pass

    def write_ram(self, addr: int, payload: bytes):
        raise NotImplementedError

    def read_ram(self, addr: int, count: int) -> bytes:
        raise NotImplementedError

    def flash_firmware(self, binary: str):
        raise NotImplementedError


_HEX_LINE = re.compile(r"^[0-9a-f]{8}:((?:\s[0-9a-f]{2})+)\s*$", re.IGNORECASE)


class MinichlinkTransport(_Transport):
    """One minichlink invocation per operation. Simple, portable, slow.

    minichlink's -w and -r halt the core for the transfer and leave it halted,
    so every call ends with -e to resume. USB re-enumeration per invocation is
    the entire cost - roughly 250 ms, against milliseconds of actual work.
    """

    name = "minichlink"
    fast = False

    def __init__(self, explicit: str = "", verbose: bool = False):
        self.exe = find_minichlink(explicit)
        self.verbose = verbose
        self.calls = 0
        fd, self._scratch = tempfile.mkstemp(suffix=".bin", prefix="chant_swio_")
        os.close(fd)

    def close(self):
        try:
            os.unlink(self._scratch)
        except OSError:
            pass

    def _run(self, args, what: str) -> str:
        cmd = [self.exe] + args
        if self.verbose:
            print("+ " + " ".join(cmd), flush=True)
        self.calls += 1
        try:
            p = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
        except subprocess.TimeoutExpired:
            raise SwioError(f"minichlink hung during {what}")
        if p.returncode != 0 or "Could not initialize" in (p.stderr or ""):
            raise SwioError(
                f"minichlink failed during {what}\n"
                f"  command: {' '.join(cmd)}\n"
                f"  stderr:  {(p.stderr or '').strip()}"
            )
        return p.stdout

    def write_ram(self, addr: int, payload: bytes):
        if len(payload) % 4:
            payload += b"\x00" * (4 - len(payload) % 4)
        with open(self._scratch, "wb") as f:
            f.write(payload)
        self._run(["-w", self._scratch, hex(addr), "-e"],
                  f"writing {len(payload)} B to {addr:#010x}")

    def read_ram(self, addr: int, count: int) -> bytes:
        out = self._run(["-r", "+", hex(addr), str(count), "-e"],
                        f"reading {count} B from {addr:#010x}")
        data = bytearray()
        for line in out.splitlines():
            m = _HEX_LINE.match(line.strip())
            if m:
                data += bytes(int(b, 16) for b in m.group(1).split())
        if len(data) < count:
            raise SwioError(f"short read from {addr:#010x}: parsed {len(data)} "
                            f"of {count} B from:\n{out}")
        return bytes(data[:count])

    def flash_firmware(self, binary: str):
        self._run(["-w", binary, "flash", "-b"],
                  f"flashing {os.path.basename(binary)}")


_MDW_WORD = re.compile(r"^0x[0-9a-f]+:((?:\s[0-9a-f]{8})+)\s*$", re.IGNORECASE)
TCL_SEP = b"\x1a"


class OpenOcdTransport(_Transport):
    """One long-lived OpenOCD, driven over its TCL port.

    This is the fast backend and the one that works on Windows with WCH's own
    driver bound to the probe. The connection stays open for the whole load, so
    a chunk costs a few milliseconds instead of a process launch.

    Writes go through `load_image`, which wants a file - so one scratch file is
    rewritten per chunk. `write_memory` would avoid that but only exists in
    OpenOCD 0.12+, and the build PlatformIO ships here reports 0.11.0+dev.
    """

    name = "openocd"
    fast = True

    def __init__(self, explicit: str = "", verbose: bool = False,
                 port: int = 6666, start_timeout: float = 20.0):
        self.exe, self.cfgdir = find_openocd(explicit)
        self.verbose = verbose
        self.port = port
        self.calls = 0
        self.keep_log = bool(os.environ.get("CHANT_KEEP_OPENOCD_LOG"))
        self.logpath = ""
        self._log = None
        self.sock = None
        self.proc = None
        fd, self._scratch = tempfile.mkstemp(suffix=".bin", prefix="chant_swio_")
        os.close(fd)
        self._start(start_timeout)

    # -- process ------------------------------------------------------------

    def _start(self, timeout: float):
        cmd = [self.exe, "-f", "wch-riscv.cfg", "-c", f"tcl_port {self.port}"]
        if self.verbose:
            print("+ " + " ".join(cmd), flush=True)
        # To a file, not a pipe. A pipe can only be read once the process
        # exits without risking a deadlock, and OpenOCD's own log is exactly
        # what you want while it is still running and misbehaving.
        fd, self.logpath = tempfile.mkstemp(suffix=".log", prefix="chant_openocd_")
        self._log = os.fdopen(fd, "w+")
        self.proc = subprocess.Popen(
            cmd, cwd=self.cfgdir,
            stdout=self._log, stderr=subprocess.STDOUT, text=True,
        )

        deadline = time.time() + timeout
        while time.time() < deadline:
            if self.proc.poll() is not None:
                raise SwioError(self._startup_error(self._read_log()))
            try:
                s = socket.create_connection(("127.0.0.1", self.port), timeout=1.0)
            except OSError:
                time.sleep(0.2)
                continue
            s.settimeout(120.0)
            self.sock = s
            # OpenOCD opens its TCL port while still bringing the target up,
            # so a successful connect proves nothing. Ask it something cheap:
            # if the probe never reached the chip, OpenOCD is already on its
            # way out and this is where we find out, with its log still to
            # hand rather than three layers deeper in a read.
            try:
                self._cmd("targets")
            except SwioError:
                self.close()
                raise
            return

        self.close()
        raise SwioError(f"OpenOCD did not open its TCL port {self.port} in "
                        f"{timeout:.0f} s")

    def _read_log(self, tail: int = 40) -> str:
        """The last `tail` lines OpenOCD wrote."""
        try:
            with open(self.logpath, "r", errors="replace") as f:
                lines = f.read().strip().splitlines()
            return "\n".join(lines[-tail:])
        except OSError:
            return ""

    def _died_message(self) -> str:
        """Whatever OpenOCD printed before it gave up, read as a diagnosis."""
        if self.proc:
            try:
                self.proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        return self._startup_error(self._read_log())

    @staticmethod
    def _startup_error(out: str) -> str:
        if "failed to connect with riscvchip" in out:
            return (
                "the WCH-LinkE opened fine but could not reach the MCU.\n"
                "That is the SWIO link itself, not this tool. Check, in order:\n"
                "  - the board is powered (the probe only supplies 3V3 if its "
                "3V3 pin is wired)\n"
                "  - SWIO on the probe goes to PD1 (U1 pin 18)\n"
                "  - GND is shared between probe and board\n"
                "  - nothing else is driving PD1\n"
                "If the chip was put to sleep or PD1 was reconfigured as GPIO, "
                "it needs the power-cycle unlock. The probe can do it:\n"
                "  wlink erase --method power-off\n"
                "(wlink ships with PlatformIO, in packages/tool-wlink. Do not "
                "reach for `minichlink -u` on this machine - minichlink cannot "
                "open this probe at all; see docs/hardware.md.)\n\n"
                "OpenOCD said:\n" + out
            )
        return "OpenOCD exited during startup:\n" + out

    def close(self):
        if self.sock:
            try:
                self._cmd("shutdown")
            except Exception:
                pass
            try:
                self.sock.close()
            except OSError:
                pass
            self.sock = None
        if self.proc:
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
            self.proc = None
        if getattr(self, "_log", None):
            try:
                self._log.close()
            except OSError:
                pass
            self._log = None
        for path in (self._scratch, getattr(self, "logpath", "")):
            if path and not self.keep_log:
                try:
                    os.unlink(path)
                except OSError:
                    pass

    # -- TCL ----------------------------------------------------------------

    def _cmd(self, command: str) -> str:
        """Send one command, return its output. OpenOCD's TCL RPC delimits
        both directions with 0x1a."""
        if not self.sock:
            raise SwioError("OpenOCD connection is closed")
        if self.verbose:
            print(f"  tcl> {command}", flush=True)
        self.calls += 1
        try:
            self.sock.sendall(command.encode() + TCL_SEP)
        except OSError:
            raise SwioError(self._died_message())

        buf = bytearray()
        while True:
            try:
                chunk = self.sock.recv(4096)
            except socket.timeout:
                raise SwioError(
                    f"OpenOCD did not answer `{command}` within "
                    f"{self.sock.gettimeout():.0f} s.\n"
                    "Mid-transfer this is usually the scratch .bin being "
                    "scanned: every chunk rewrites it, and a real-time virus "
                    "scanner can hold the file shut for seconds. Excluding "
                    "%TEMP%/chant_swio_*.bin fixes it. Running the load again "
                    "is safe - it erases and rewrites from address 0.\n"
                    "OpenOCD's log said:\n" + self._read_log())
            except OSError:
                # Connection reset: OpenOCD exited mid-command. Its own log
                # says why, and it is almost always the probe-to-chip link.
                raise SwioError(self._died_message())
            if not chunk:
                raise SwioError(self._died_message())
            buf += chunk
            if buf.endswith(TCL_SEP):
                break
        return buf[:-1].decode(errors="replace")

    def _checked(self, command: str) -> str:
        out = self._cmd(command)
        # TCL errors come back as text; OpenOCD has no status channel here.
        low = out.lower()
        if "error" in low or "invalid command" in low:
            raise SwioError(f"OpenOCD rejected `{command}`:\n{out.strip()}")
        return out

    # -- memory -------------------------------------------------------------

    def write_ram(self, addr: int, payload: bytes):
        if len(payload) % 4:
            payload += b"\x00" * (4 - len(payload) % 4)
        with open(self._scratch, "wb") as f:
            f.write(payload)
        path = self._scratch.replace("\\", "/")   # TCL eats backslashes
        self._checked("halt")
        self._checked(f"load_image {{{path}}} {addr:#x} bin")
        self._checked("resume")

    def read_ram(self, addr: int, count: int) -> bytes:
        if addr % 4 or count % 4:
            raise SwioError("the OpenOCD backend reads whole words only")
        self._checked("halt")
        out = self._checked(f"mdw {addr:#x} {count // 4}")
        self._checked("resume")

        words = []
        for line in out.splitlines():
            m = _MDW_WORD.match(line.strip())
            if m:
                words += [int(w, 16) for w in m.group(1).split()]
        if len(words) < count // 4:
            raise SwioError(f"short read from {addr:#010x}: parsed "
                            f"{len(words)} of {count // 4} words from:\n{out}")
        return struct.pack(f"<{count // 4}I", *words[:count // 4])

    def flash_firmware(self, image: str):
        """Write a firmware image to MCU flash and run it.

        An ELF carries its own load addresses, so `program` needs none. A raw
        .bin needs one, and it is 0x00000000, not 0x08000000: wch-riscv.cfg
        declares the bank at the alias ("flash bank ... 0x00000000"), and
        OpenOCD matches the address against the bank it was given rather than
        the physical one.
        """
        path = os.path.abspath(image).replace("\\", "/")
        where = "" if path.lower().endswith(".elf") else " 0x00000000"
        self._checked("halt")
        self._checked(f"program {{{path}}}{where} verify")
        self._checked("reset run")
        time.sleep(0.3)


def make_transport(backend: str = "auto", path: str = "",
                   verbose: bool = False) -> _Transport:
    """Pick a backend. 'auto' prefers OpenOCD and says why if it falls back."""
    if backend == "openocd":
        return OpenOcdTransport(path, verbose)
    if backend == "minichlink":
        return MinichlinkTransport(path, verbose)
    if backend != "auto":
        raise SwioError(f"unknown backend {backend!r}")

    try:
        return OpenOcdTransport(path, verbose)
    except SwioError as e:
        # A probe that cannot reach the chip is a real fault, not a reason to
        # try the other backend - minichlink would fail too, more obscurely.
        if "could not reach the MCU" in str(e):
            raise
        print(f"note: OpenOCD backend unavailable ({str(e).splitlines()[0]}); "
              "falling back to minichlink", file=sys.stderr)
        return MinichlinkTransport(path, verbose)


# ---- the mailbox ----------------------------------------------------------

class SwioLink:
    def __init__(self, backend: str = "auto", path: str = "",
                 verbose: bool = False):
        self.io = make_transport(backend, path, verbose)
        self.seq = 0

    @property
    def calls(self) -> int:
        return self.io.calls

    @property
    def backend(self) -> str:
        return self.io.name

    @property
    def fast(self) -> bool:
        return self.io.fast

    def close(self):
        self.io.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()
        return False

    def flash_firmware(self, binary: str):
        if not os.path.isfile(binary):
            raise SwioError(f"no firmware image at {binary}")
        self.io.flash_firmware(binary)

    def read_header(self) -> dict:
        f = struct.unpack("<8I", self.io.read_ram(MBOX_ADDR, MBOX_HDR_SIZE))
        return dict(magic=f[0], cmd=f[1], addr=f[2], len=f[3],
                    seq=f[4], ack=f[5], status=f[6], reply=f[7])

    def wait_for_loader(self, timeout: float = 10.0) -> dict:
        """Block until the loader has booted and published its magic word."""
        deadline = time.time() + timeout
        last = None
        while time.time() < deadline:
            last = self.read_header()
            if last["magic"] == MBOX_MAGIC:
                return last
            time.sleep(0.2)
        raise SwioError(
            f"the SWIO loader never came up: expected {MBOX_MAGIC:#010x} at "
            f"{MBOX_ADDR:#010x}, last saw {(last or {}).get('magic', 0):#010x}.\n"
            "Is loader_swio actually on the MCU, and is the board powered?"
        )

    def request(self, cmd: int, addr: int = 0, length: int = 0,
                data: bytes = b"", wait: bool = True,
                timeout: float = 5.0) -> dict:
        """Post one request; with wait=False, do not read the reply back.

        wait=False is what makes the minichlink backend tolerable: it halves
        the invocations, and a 512-byte program finishes tens of times over
        before the next chunk can arrive. On the OpenOCD backend a read costs
        almost nothing, so load_audio_swio waits on everything.
        """
        if len(data) > MBOX_DATA_MAX:
            raise SwioError(f"{len(data)} B is more than the mailbox holds")

        self.seq += 1
        blob = struct.pack("<8I", MBOX_MAGIC, cmd, addr, length,
                           self.seq, 0, 0, 0) + data
        self.io.write_ram(MBOX_ADDR, blob)

        if not wait:
            return {}

        deadline = time.time() + timeout
        while time.time() < deadline:
            h = self.read_header()
            if h["ack"] == self.seq and h["cmd"] == 0:
                if h["status"] != ST_OK:
                    raise SwioError(
                        f"loader rejected command {cmd} at {addr:#08x}: "
                        f"{ST_NAMES.get(h['status'], h['status'])}")
                return h
            time.sleep(0.01 if self.fast else 0.05)

        raise SwioError(
            f"no answer to command {cmd} within {timeout:.0f} s - the loader "
            "stopped running. Power-cycle the board and start again.")

    # -- convenience --------------------------------------------------------

    def ping(self) -> int:
        return self.request(CMD_PING)["reply"]

    def jedec_id(self) -> int:
        return self.request(CMD_JEDEC)["reply"]

    def unprotect(self):
        self.request(CMD_UNPROTECT, timeout=5.0)

    def erase_sector(self, addr: int):
        # tSE on an FM25Q08B is up to 300 ms; 5 s is slack, not an expectation.
        self.request(CMD_ERASE, addr=addr, timeout=5.0)

    def program(self, addr: int, data: bytes, wait: bool = False):
        self.request(CMD_PROGRAM, addr=addr, length=len(data), data=data,
                     wait=wait)

    def crc32(self, addr: int, length: int, timeout: float = 60.0) -> int:
        return self.request(CMD_CRC32, addr=addr, length=length,
                            timeout=timeout)["reply"]


if __name__ == "__main__":
    # Smoke test, in the spirit of bring-up stage 5: is the probe there, is the
    # loader running, does the flash answer?
    import argparse

    ap = argparse.ArgumentParser(description="SWIO loader smoke test")
    ap.add_argument("--backend", default="auto",
                    choices=["auto", "openocd", "minichlink"])
    ap.add_argument("--path", default="", help="full path to the backend tool")
    ap.add_argument("-v", "--verbose", action="store_true")
    a = ap.parse_args()

    try:
        with SwioLink(a.backend, a.path, a.verbose) as link:
            print(f"backend: {link.backend}")
            link.wait_for_loader()
            ver = link.ping()
            jid = link.jedec_id()
            print(f"loader version {ver:#010x} "
                  f"({'ok' if ver == LOADER_VERSION else 'UNEXPECTED'})")
            print(f"JEDEC id      {jid:#08x} "
                  f"({'ok' if jid == JEDEC_EXPECTED else 'UNEXPECTED'})")
    except SwioError as e:
        sys.exit(f"error: {e}")
