"""size command — measure flash/RAM footprint of a built PlatformIO/ESP-IDF
env, device-free (no flashing).

Surface: bbtool size --build-dir .pio/build/<env> [--component bb_wifi ...]

Runs the target toolchain's `size` binary against the env's `.elf` for
aggregate flash text/data + RAM bss (mirrors decode_lib.py's toolchain
auto-detect convention — `find_size_tool()` is `find_addr2line()`'s sibling),
then parses the `.map` file (same build dir) for a per-component archive-
member breakdown — sums bytes attributed to each `libbb_<name>.a(...)` object
across all sections. Best-effort/heuristic (a `.map` file's section-entry
format is toolchain-version-sensitive), sufficient to answer whether
excluding a component from a REQUIRES list actually dropped it from the
linked image.

Emits one structured JSON line per invocation (`print(json.dumps(...))`) —
the format both `bbtool elf` and `bbtool stage` output info; JSON was chosen
here specifically so build-vs-build size diffs can be scripted/diffed
without a bespoke parser.
"""
from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Dict, List, Optional

NAME = "size"
HELP = "measure ELF flash/RAM footprint + per-component .map breakdown (device-free)"


def add_arguments(parser) -> None:
    parser.add_argument(
        "--build-dir",
        dest="build_dir",
        required=True,
        metavar="DIR",
        help="PlatformIO/ESP-IDF build dir containing firmware.elf (+ firmware.map)",
    )
    parser.add_argument(
        "--elf",
        dest="elf_path",
        default=None,
        metavar="PATH",
        help="explicit .elf path (default: <build-dir>/firmware.elf)",
    )
    parser.add_argument(
        "--map",
        dest="map_path",
        default=None,
        metavar="PATH",
        help="explicit .map path (default: <build-dir>/firmware.map)",
    )
    parser.add_argument(
        "--component",
        dest="components",
        action="append",
        default=None,
        metavar="NAME",
        help="component name to report a .map size breakdown for (repeatable)",
    )
    parser.add_argument(
        "--arch",
        default="xtensa",
        choices=["xtensa", "riscv"],
        help="toolchain arch for the size binary (default: xtensa)",
    )
    parser.add_argument(
        "--size-tool",
        dest="size_tool_path",
        default=None,
        metavar="PATH",
        help="explicit size binary (overrides auto-detect)",
    )


def register(api) -> None:
    api.add_command(NAME, sys.modules[__name__])


# ---------------------------------------------------------------------------
# Toolchain discovery — sibling of decode_lib.find_addr2line
# ---------------------------------------------------------------------------

def find_size_tool(arch: str = "xtensa", size_tool_path: Optional[str] = None) -> Optional[str]:
    """Locate the `size` binary for the given arch.

    Search order:
      1. size_tool_path arg (explicit override)
      2. BBTOOL_SIZE_TOOL env var
      3. ~/.platformio/packages/toolchain-*/bin/
      4. System PATH

    Tries both the newer `xtensa-esp-elf-size` / `riscv32-esp-elf-size` naming
    and the older `xtensa-esp32-elf-size` naming, in that order (mirrors
    decode_lib.find_addr2line's toolchain-name search).
    """
    if arch == "riscv":
        names = ["riscv32-esp-elf-size"]
    else:
        names = ["xtensa-esp-elf-size", "xtensa-esp32-elf-size"]

    candidates = [size_tool_path, os.environ.get("BBTOOL_SIZE_TOOL")]
    for candidate in candidates:
        if candidate and os.path.isfile(candidate) and os.access(candidate, os.X_OK):
            return candidate

    pio_packages = Path.home() / ".platformio" / "packages"
    if pio_packages.is_dir():
        for tc_dir in sorted(pio_packages.glob("toolchain-*")):
            bin_dir = tc_dir / "bin"
            for name in names:
                p = bin_dir / name
                if p.is_file() and os.access(str(p), os.X_OK):
                    return str(p)

    for name in names:
        found = shutil.which(name)
        if found:
            return found

    return None


# ---------------------------------------------------------------------------
# `size` output parsing (Berkeley format: text data bss dec hex filename)
# ---------------------------------------------------------------------------

def parse_size_output(output: str) -> Dict[str, int]:
    """Parse Berkeley-format `size` output's second (data) line into
    {text, data, bss, dec}. Raises ValueError if the expected two-line shape
    isn't found."""
    lines = [ln for ln in output.splitlines() if ln.strip()]
    if len(lines) < 2:
        raise ValueError(f"unexpected `size` output (no data line): {output!r}")
    fields = lines[1].split()
    if len(fields) < 4:
        raise ValueError(f"unexpected `size` data line: {lines[1]!r}")
    return {
        "text": int(fields[0]),
        "data": int(fields[1]),
        "bss": int(fields[2]),
        "dec": int(fields[3]),
    }


# ---------------------------------------------------------------------------
# .map file per-component breakdown
# ---------------------------------------------------------------------------

# Matches a linker-map section-entry line attributing a sized chunk to an
# archive member, e.g.:
#   .text.bb_wifi_init
#                  0x0000000040123456      0x120 .pio/.../libbb_wifi.a(bb_wifi.c.o)
# Section name/address may be on the previous physical line (GNU ld wraps long
# section names), so this matches on the (address, size, archive) triple only
# — the two hex numbers followed by a path ending in lib<name>.a(...).
_MAP_MEMBER_RE = re.compile(
    r"0x[0-9a-fA-F]+\s+0x([0-9a-fA-F]+)\s+\S*lib(bb_[A-Za-z0-9_]+)\.a\(",
)


def parse_map_component_sizes(map_text: str, components: Optional[List[str]] = None) -> Dict[str, int]:
    """Sum bytes attributed to each `libbb_<name>.a(...)` archive member
    across every section entry in the map file. Returns {component_name:
    total_bytes} — only for components present in `components` when given,
    else every `bb_*` archive found. Best-effort heuristic; a component with
    zero matches (e.g. genuinely excluded from the link) is simply absent
    from the returned dict, not present with size 0 — callers use `.get(name,
    0)` to distinguish "excluded" from "included but empty"."""
    totals: Dict[str, int] = {}
    for m in _MAP_MEMBER_RE.finditer(map_text):
        size_hex, comp_name = m.group(1), m.group(2)
        if components is not None and comp_name not in components:
            continue
        totals[comp_name] = totals.get(comp_name, 0) + int(size_hex, 16)
    return totals


# ---------------------------------------------------------------------------
# Command entry point
# ---------------------------------------------------------------------------

def run(args) -> int:
    build_dir = os.path.abspath(args.build_dir)
    elf_path = args.elf_path or os.path.join(build_dir, "firmware.elf")
    map_path = args.map_path or os.path.join(build_dir, "firmware.map")

    if not os.path.isfile(elf_path):
        print(f"bbtool size: error: ELF not found: {elf_path}", file=sys.stderr)
        return 1

    size_tool = find_size_tool(args.arch, args.size_tool_path)
    if size_tool is None:
        print(
            f"bbtool size: error: no size tool found for arch '{args.arch}' "
            "(install ESP-IDF toolchain or set BBTOOL_SIZE_TOOL)",
            file=sys.stderr,
        )
        return 1

    try:
        proc = subprocess.run(
            [size_tool, "--format=berkeley", elf_path],
            capture_output=True, text=True, timeout=30, check=True,
        )
    except subprocess.CalledProcessError as exc:
        print(f"bbtool size: error: {size_tool} failed (rc={exc.returncode}): {exc.stderr}", file=sys.stderr)
        return 1
    except subprocess.TimeoutExpired:
        print(f"bbtool size: error: {size_tool} timed out", file=sys.stderr)
        return 1

    try:
        agg = parse_size_output(proc.stdout)
    except ValueError as exc:
        print(f"bbtool size: error: {exc}", file=sys.stderr)
        return 1

    components: Dict[str, int] = {}
    if os.path.isfile(map_path):
        map_text = Path(map_path).read_text(encoding="utf-8", errors="replace")
        components = parse_map_component_sizes(map_text, args.components)
    else:
        print(f"bbtool size: warning: .map not found ({map_path}); no per-component breakdown", file=sys.stderr)

    result = {
        "elf": elf_path,
        "build_dir": build_dir,
        "text": agg["text"],
        "data": agg["data"],
        "bss": agg["bss"],
        "flash_total": agg["text"] + agg["data"],
        "components": components,
    }
    print(json.dumps(result, sort_keys=True))
    return 0
