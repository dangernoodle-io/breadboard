"""Thin entry point — delegates to scripts/bbtool/cli.py."""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "bbtool"))

from cli import main  # noqa: E402

if __name__ == "__main__":
    sys.exit(main())
