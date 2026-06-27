"""embed_html.py — FORWARDING SHIM.

The implementation has moved to scripts/bbtool/commands/embed.py.
This file is kept so that existing test/script callers continue to work.
Do not add new logic here. Migrate callers to bbtool embed directly.
"""
import os
import sys

# Ensure bbtool package is importable
_BBTOOL_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "bbtool")
if _BBTOOL_DIR not in sys.path:
    sys.path.insert(0, _BBTOOL_DIR)

from commands.embed import embed_file, _embed as embed  # noqa: E402,F401

# Re-export the public API at module level for backward-compat imports
__all__ = ["embed_file", "embed"]


def main():
    if len(sys.argv) != 4:
        print(
            "usage: embed_html.py <input_path> <output_c_path> <symbol_name>",
            file=sys.stderr,
        )
        sys.exit(1)

    _, input_path, output_c_path, symbol_name = sys.argv
    embed(input_path, output_c_path, symbol_name)


if __name__ == "__main__":
    main()
