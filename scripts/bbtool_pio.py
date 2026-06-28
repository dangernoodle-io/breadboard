"""
bbtool_pio.py — breadboard's canonical PlatformIO pre-script hook.

Handles firmware version header generation for breadboard consumers, delegating
to bbtool.commands.version. Wire it up in your platformio.ini:

  extra_scripts = pre:.breadboard/scripts/bbtool_pio.py

Logic lives in scripts/bbtool/commands/version.py.
"""
import inspect
import os
import sys

# Capture real path at module load time (SCons exec() doesn't set __file__).
_THIS_FILE = os.path.abspath(inspect.currentframe().f_code.co_filename)
_THIS_DIR  = os.path.dirname(os.path.realpath(_THIS_FILE))

# Add scripts/bbtool to sys.path so 'from commands.version import ...' works.
_BBTOOL_DIR = os.path.join(_THIS_DIR, "bbtool")
if _BBTOOL_DIR not in sys.path:
    sys.path.insert(0, _BBTOOL_DIR)

if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser(description="Compute bb firmware version string")
    from commands.version import add_arguments, run
    add_arguments(parser)
    args = parser.parse_args()
    import sys as _sys
    _sys.exit(run(args))
else:
    # SCons/PlatformIO exec path
    from commands.version import pio_main
    try:
        Import("env")  # noqa: F821
        pio_main(env)  # noqa: F821
    except NameError:
        pass
