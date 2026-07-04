"""Thin entry point — delegates to scripts/bbdevice/cli.py."""
import os
import sys

# Put the PARENT scripts/ dir on sys.path so bbdevice is importable as a
# package (`import bbdevice.cli`) with absolute intra-package imports — this
# avoids sys.modules collisions with bbtool's identically-named flat modules
# (core/registry/cli/commands).
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from bbdevice.cli import main  # noqa: E402

if __name__ == "__main__":
    sys.exit(main())
