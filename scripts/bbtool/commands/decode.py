"""decode command — decode a panic backtrace from a live device.

Surface:  bbtool decode HOST [--elf PATH] [--archive-dir DIR] [--toolchain-path PATH]

Flow:
  1. GET http://HOST/api/diag/panic  -> panic dict (app_sha256, exc_pc, backtrace, task, exc_cause)
  2. GET http://HOST/api/info        -> build.chip_model -> chip_arch()
  3. Resolve ELF: --elf explicit, else elfstore.find(app_sha256)
  4. decode_panic() -> print labeled frame table
"""
from __future__ import annotations

import sys

NAME = "decode"
HELP = "decode a panic backtrace from a live device (uses archived ELF)"


def add_arguments(parser) -> None:
    parser.add_argument("host", metavar="HOST",
                        help="device IP or hostname (http://HOST/api/diag/panic)")
    parser.add_argument("--elf", dest="elf_path", metavar="PATH",
                        help="explicit ELF file (overrides archive lookup)")
    parser.add_argument("--archive-dir", dest="archive_dir", default=None, metavar="DIR",
                        help="ELF archive root (overrides config / BBTOOL_ELF_ARCHIVE)")
    parser.add_argument("--toolchain-path", dest="toolchain_path", metavar="PATH",
                        help="explicit addr2line binary (overrides auto-detect)")


def register(api) -> None:
    api.add_command(NAME, sys.modules[__name__])


# ---------------------------------------------------------------------------
# HTTP helpers (stdlib urllib — no third-party deps)
# ---------------------------------------------------------------------------

def _get_json(host: str, path: str, timeout: int = 10):
    """GET http://HOST/path, return parsed JSON dict or None on error."""
    import json as _json
    import urllib.request
    url = f"http://{host}{path}"
    try:
        with urllib.request.urlopen(url, timeout=timeout) as r:
            return _json.loads(r.read())
    except Exception as exc:
        import logging
        logging.getLogger(__name__).debug("GET %s failed: %s", url, exc)
        return None


# ---------------------------------------------------------------------------
# ELF resolution helper (mirrors elf.py precedence)
# ---------------------------------------------------------------------------

def _resolve_elf(args, app_sha: str) -> str | None:
    """Return path to ELF or None.  --elf wins; else elfstore.find() with archive_dir."""
    elf_path = getattr(args, "elf_path", None)
    if elf_path:
        return elf_path

    if not app_sha:
        return None

    import elfstore
    archive_dir = getattr(args, "archive_dir", None) or (
        getattr(args, "_config_dict", {}).get("elf", {}).get("archive_dir")
    )
    return elfstore.find(app_sha, archive_dir=archive_dir)


# ---------------------------------------------------------------------------
# Command entry point
# ---------------------------------------------------------------------------

def run(args) -> int:
    from decode_lib import chip_arch, decode_panic

    host = args.host.rstrip("/")
    # Strip any leading http:// the user may have typed
    if host.startswith("http://"):
        host = host[len("http://"):]
    elif host.startswith("https://"):
        host = host[len("https://"):]

    # --- fetch panic ---
    panic = _get_json(host, "/api/diag/panic")
    if panic is None:
        print(f"ERROR: could not reach {host} or /api/diag/panic unavailable")
        return 1

    available = panic.get("available", False)
    app_sha   = panic.get("app_sha256", "")
    has_data  = panic.get("backtrace") or panic.get("exc_pc")

    if not available and not has_data:
        print(f"{host}: no panic available")
        return 0

    # --- chip arch from /api/info ---
    info = _get_json(host, "/api/info") or {}
    chip_model = (info.get("build") or {}).get("chip_model", "") or "ESP32"
    arch = chip_arch(chip_model)

    # --- resolve ELF ---
    elf_path = _resolve_elf(args, app_sha)
    if elf_path is None:
        if app_sha:
            print(f"ERROR: no archived ELF for '{app_sha}'; "
                  "reflash with a tracked build (bbtool elf archive) or pass --elf <path>")
        else:
            print("ERROR: no app_sha256 in panic response and no --elf given")
        return 1

    # --- decode ---
    toolchain_path = getattr(args, "toolchain_path", None)
    result = decode_panic(panic, elf_path, arch=arch, toolchain_path=toolchain_path)

    # --- print ---
    print(f"\nPanic decode for {host}")
    print(f"  ELF     : {elf_path}")
    print(f"  arch    : {arch}")
    print(f"  task    : {result.task or '?'}")
    print(f"  cause   : {result.exc_cause} ({result.cause_name_str})")
    if app_sha:
        print(f"  sha256  : {app_sha}")
    if not result.ok:
        print(f"  ERROR   : {result.error}")
        return 1

    if not result.frames:
        print("  (no frames decoded)")
    else:
        print(f"\n  {'LABEL':<10} {'PC':>12}   FUNCTION @ FILE:LINE")
        print(f"  {'-' * 70}")
        for label, pc, frame in result.frames:
            print(f"  {label:<10} {pc:#012x}   {frame}")

    return 0
