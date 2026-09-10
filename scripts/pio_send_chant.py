"""
pio_send_chant.py - adds `pio run -e loader -t sendchant` to the loader env.

A thin wrapper around tools/send_chant.py so the whole audio-loading workflow
lives in one tool once you are working inside PlatformIO:

    pio run -e loader -t upload      put the loader on the MCU
    pio run -e loader -t sendchant   push chant.raw into the SPI flash
    pio run -e player -t upload      put the player back

It is a convenience only. tools/send_chant.py stands alone and works the same
from a plain shell, which is what the Makefile route uses.

Configured by the custom_chant_* options in platformio.ini's [env:loader].
"""

import os
import subprocess
import sys

Import("env")  # noqa: F821 - injected by PlatformIO/SCons


def _opt(env, name, default=""):
    try:
        return (env.GetProjectOption(name, default) or "").strip()
    except Exception:
        return default


def send_chant(target, source, env):
    project = env["PROJECT_DIR"]
    script = os.path.join(project, "tools", "send_chant.py")

    if not os.path.isfile(script):
        print(f"error: {script} is missing")
        return 1

    audio = _opt(env, "custom_chant_file", "chant.raw") or "chant.raw"
    if not os.path.isabs(audio):
        audio = os.path.join(project, audio)
    if not os.path.isfile(audio):
        print(
            f"error: no audio file at {audio}\n"
            "  Convert a recording first, for example:\n"
            "    ffmpeg -i chant.wav -ac 1 -ar 8000 -f u8 -acodec pcm_u8 chant.raw\n"
            "  or point custom_chant_file in platformio.ini somewhere else."
        )
        return 1

    # The serial port of the USB-serial adapter wired to PD5/PD6. This is not
    # the debug probe's SWIO interface, and PlatformIO's upload_port refers to
    # the latter, so it is deliberately not consulted here.
    port = (
        _opt(env, "custom_chant_port")
        or _opt(env, "monitor_port")
        or os.environ.get("CHANT_PORT", "").strip()
    )
    if not port:
        print(
            "error: no serial port set.\n"
            "  Set custom_chant_port (or monitor_port) in platformio.ini's\n"
            "  [env:loader], or export CHANT_PORT. `pio device list` shows what\n"
            "  is attached. On a WCH-LinkE this is its serial interface, not the\n"
            "  debug one."
        )
        return 1

    extra = _opt(env, "custom_chant_args").split()

    # PlatformIO's own interpreter ships pyserial, so this needs no extra
    # install - which is the main reason to shell out to it rather than import.
    cmd = [sys.executable, script, port, audio] + extra
    print(" ".join(cmd))
    return subprocess.call(cmd)


env.AddCustomTarget(  # noqa: F821
    name="sendchant",
    dependencies=None,
    actions=[send_chant],
    title="Send chant to flash",
    description="Write the chant into U3 through the loader firmware",
    always_build=True,
)
