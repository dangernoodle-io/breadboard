"""gen_site.py — FORWARDING SHIM.

The implementation has moved to scripts/bbtool/commands/gen_site.py.
This file is kept so that existing test/script callers continue to work.
Do not add new logic here. Migrate callers to bbtool gen-site directly.
"""
import os
import sys

# Ensure bbtool package is importable
_BBTOOL_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "bbtool")
if _BBTOOL_DIR not in sys.path:
    sys.path.insert(0, _BBTOOL_DIR)

from commands.gen_site import (  # noqa: E402,F401
    MIME_MAP,
    _DEFAULT_MIME,
    _mime,
    _sanitize,
    _symbol,
    _url,
    _collect_files,
    _write_table,
    generate,
)

# Re-export for backward-compat
__all__ = [
    "MIME_MAP", "_DEFAULT_MIME", "_mime", "_sanitize", "_symbol",
    "_url", "_collect_files", "_write_table", "generate",
]


def main():
    import argparse
    parser = argparse.ArgumentParser(
        description="Embed a built-SPA dist/ directory into firmware as gzipped C blobs."
    )
    parser.add_argument("dist_dir",  help="Path to the dist directory")
    parser.add_argument("out_dir",   help="Directory for generated .c files")
    parser.add_argument("table_sym", help="C identifier prefix for the asset table")
    parser.add_argument("--url-prefix", default="/",
                        help="URL root prepended to asset paths (default '/')")
    args = parser.parse_args()

    generated = generate(args.dist_dir, args.out_dir, args.table_sym,
                         url_prefix=args.url_prefix)

    # Print absolute paths to stdout — one per line — for CMake to capture.
    for path in generated:
        print(path)


if __name__ == "__main__":
    main()
