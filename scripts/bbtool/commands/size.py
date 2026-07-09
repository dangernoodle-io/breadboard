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

B1-719 phase A — static footprint baseline/gate (flash only; heap is phase
B and stays inert/null here):

    bbtool size --update-baseline --target esp32 --build-dir .pio/build/esp32
    bbtool size --check           --target esp32 --build-dir .pio/build/esp32

`--update-baseline` measures the current build, snapshots + normalizes the
resolved sdkconfig (every `CONFIG_*` knob, not just `CONFIG_BB_*`), and
writes `.baseline/bbtool/metrics/<target>.json` (+ a sibling
`.sdkconfig` snapshot file). Any existing `heap` block in that file is
preserved verbatim on rewrite — this command never measures live heap, so
it must never clobber a hand/device-populated one.

`--check` re-measures and compares against the committed baseline: `bss` is
shrink-only (any growth is a FAIL), `flash_total` may grow up to
`--flash-threshold-pct` (default 2.0) before it's a FAIL. This is the FLASH
gate only; the heap gate below is separate and stays inert while the
baseline's `heap` block is null.

B1-719 phase B — live device heap capture + gate (device/fleet-run only,
needs a live board reachable over HTTP):

    bbtool size --update-baseline --target esp32 --build-dir .pio/build/esp32 \
        --heap-from-http http://<device-ip>
    bbtool size --check           --target esp32 --build-dir .pio/build/esp32 \
        --heap-from-http http://<device-ip>

`--heap-from-http <base_url>` GETs `<base_url>/api/diag/heap` (the bb_diag
component's per-capability heap endpoint) and, with `--update-baseline`,
snapshots `internal.minimum_ever_free` into `heap.min_free`/`heap.high_water`
(the firmware exposes one watermark field, so both mirror it) plus a
per-region map into `heap.regions`. With `--check`, it re-fetches and applies
a HIGHER-BETTER regression gate (current < baseline = FAIL) against the
committed baseline's `heap` block; if that block is null (no heap baseline
yet), the heap check is inert (skipped, not failed).
"""
from __future__ import annotations

import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import urllib.error
import urllib.request
from pathlib import Path
from typing import Dict, List, Optional, Tuple

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
    parser.add_argument(
        "--root",
        default=None,
        help="repository root, for resolving .baseline/ (default: from top-level --root or cwd)",
    )
    parser.add_argument(
        "--target",
        default=None,
        metavar="NAME",
        help="baseline target label (default: build-dir basename, e.g. 'esp32')",
    )
    parser.add_argument(
        "--update-baseline",
        dest="update_baseline",
        action="store_true",
        help="measure + snapshot the resolved sdkconfig, write"
             " .baseline/bbtool/metrics/<target>.json (preserves any existing"
             " heap block)",
    )
    parser.add_argument(
        "--check",
        action="store_true",
        help="measure + compare against the committed baseline; FAIL on bss"
             " growth or flash_total growth beyond --flash-threshold-pct",
    )
    parser.add_argument(
        "--flash-threshold-pct",
        dest="flash_threshold_pct",
        type=float,
        default=2.0,
        metavar="PCT",
        help="allowed flash_total growth vs baseline, in percent (default: 2.0)",
    )
    parser.add_argument(
        "--heap-from-http",
        dest="heap_from_http",
        default=None,
        metavar="BASE_URL",
        help="fetch live heap stats from <BASE_URL>/api/diag/heap (bb_diag"
             " component) and, with --update-baseline, snapshot them into the"
             " target's heap block, or with --check, gate current heap"
             " watermarks against the committed baseline (device/fleet-run"
             " only; requires a live board)",
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
# Measurement — factored out of run() so --check/--update-baseline (and
# tests) can drive it without shelling out to a real toolchain; run() itself
# also uses it for the default single-shot JSON path.
# ---------------------------------------------------------------------------

def _measure(
    build_dir: str,
    elf_path: str,
    map_path: str,
    components: Optional[List[str]],
    arch: str,
    size_tool_path: Optional[str],
) -> Tuple[Optional[dict], Optional[str], Optional[str]]:
    """Run the toolchain `size` binary + .map parse. Returns (result, error,
    warning): `result` is the {elf, build_dir, text, data, bss, flash_total,
    components} dict (None on error); `error` is a message for the caller to
    print+fail on (None on success); `warning` is a non-fatal message (e.g.
    missing .map) to surface alongside a successful result."""
    if not os.path.isfile(elf_path):
        return None, f"ELF not found: {elf_path}", None

    size_tool = find_size_tool(arch, size_tool_path)
    if size_tool is None:
        return None, (
            f"no size tool found for arch '{arch}' "
            "(install ESP-IDF toolchain or set BBTOOL_SIZE_TOOL)"
        ), None

    try:
        proc = subprocess.run(
            [size_tool, "--format=berkeley", elf_path],
            capture_output=True, text=True, timeout=30, check=True,
        )
    except subprocess.CalledProcessError as exc:
        return None, f"{size_tool} failed (rc={exc.returncode}): {exc.stderr}", None
    except subprocess.TimeoutExpired:
        return None, f"{size_tool} timed out", None

    try:
        agg = parse_size_output(proc.stdout)
    except ValueError as exc:
        return None, str(exc), None

    comps: Dict[str, int] = {}
    warning: Optional[str] = None
    if os.path.isfile(map_path):
        map_text = Path(map_path).read_text(encoding="utf-8", errors="replace")
        comps = parse_map_component_sizes(map_text, components)
    else:
        warning = f".map not found ({map_path}); no per-component breakdown"

    result = {
        "elf": elf_path,
        "build_dir": build_dir,
        "text": agg["text"],
        "data": agg["data"],
        "bss": agg["bss"],
        "flash_total": agg["text"] + agg["data"],
        "components": comps,
    }
    return result, None, warning


# ---------------------------------------------------------------------------
# sdkconfig snapshot — captures EVERY CONFIG_* knob (native ESP-IDF like
# CONFIG_LWIP_IPV6/log level/PSRAM/stacks, not just CONFIG_BB_*), normalized
# to a deterministic sorted form so a knob flip is a visible line diff.
# ---------------------------------------------------------------------------

_CONFIG_SET_RE = re.compile(r'^(CONFIG_[A-Za-z0-9_]+)=(.*)$')
_CONFIG_UNSET_RE = re.compile(r'^# (CONFIG_[A-Za-z0-9_]+) is not set$')


def _normalize_sdkconfig(text: str) -> str:
    """Normalize raw sdkconfig text: every `CONFIG_X=value` line kept as-is;
    every `# CONFIG_X is not set` line is rewritten as the explicit sentinel
    `CONFIG_X=n` (ESP-IDF/Kconfig itself never emits a literal `=n` value
    for a bool/tristate, so this can't collide with a real value) — a knob
    flip between "set" and "not set" is then a visible line-level diff, not
    a silent line removal/addition. Every other comment/blank line is
    dropped. Lines are deduplicated + sorted (on-disk order is a
    Kconfig-tree-walk artifact, not meaningful) for determinism."""
    lines = set()
    for raw in text.splitlines():
        line = raw.strip()
        if not line:
            continue
        m = _CONFIG_SET_RE.match(line)
        if m:
            lines.add(f"{m.group(1)}={m.group(2)}")
            continue
        m = _CONFIG_UNSET_RE.match(line)
        if m:
            lines.add(f"{m.group(1)}=n")
            continue
    return "\n".join(sorted(lines)) + "\n"


def _find_sdkconfig(build_dir: Path) -> Optional[Path]:
    """Locate the resolved textual sdkconfig for a PlatformIO/ESP-IDF build
    dir. Search order: a raw build-dir copy (<build_dir>/sdkconfig, as a
    plain ESP-IDF `idf.py build` layout would have), then PlatformIO's
    env-specific project-root copy (<project_dir>/sdkconfig.<env>, where
    project_dir is 3 levels up from <project_dir>/.pio/build/<env>), then a
    bare project-root sdkconfig as a last resort. Returns None if none
    exist (callers snapshot an empty config rather than erroring — a
    missing sdkconfig is itself a visible, diffable baseline state)."""
    build_dir = Path(build_dir)
    env = build_dir.name
    candidates = [build_dir / "sdkconfig"]
    if len(build_dir.parts) >= 3:
        project_dir = build_dir.parents[2]
        candidates.append(project_dir / f"sdkconfig.{env}")
        candidates.append(project_dir / "sdkconfig")
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    return None


def _snapshot_config(build_dir: str, target: str, root: str) -> Tuple[str, str]:
    """Read + normalize the resolved sdkconfig for build_dir, write it to
    .baseline/bbtool/metrics/<target>.sdkconfig, and return (sha256_hex of
    the normalized text, snapshot path relative to root, POSIX-style)."""
    sdkconfig_path = _find_sdkconfig(Path(build_dir))
    raw_text = (
        sdkconfig_path.read_text(encoding="utf-8", errors="replace")
        if sdkconfig_path is not None else ""
    )
    normalized = _normalize_sdkconfig(raw_text)
    sha = hashlib.sha256(normalized.encode("utf-8")).hexdigest()

    snapshot_path = _baseline_dir(root) / f"{target}.sdkconfig"
    snapshot_path.parent.mkdir(parents=True, exist_ok=True)
    _atomic_write_text(snapshot_path, normalized)

    return sha, f".baseline/bbtool/metrics/{target}.sdkconfig"


# ---------------------------------------------------------------------------
# Baseline load/write — one JSON file per target at
# .baseline/bbtool/metrics/<target>.json (sibling convention to the `fence`
# family's .baseline/bbtool/fence/<family>.json, but its own top-level
# "metrics" family since this isn't a marker-occurrence scan).
# ---------------------------------------------------------------------------

_HEAP_NULL = {"min_free": None, "high_water": None, "regions": None, "source": None}


def _baseline_dir(root: str) -> Path:
    return Path(root) / ".baseline" / "bbtool" / "metrics"


def baseline_path(root: str, target: str) -> Path:
    return _baseline_dir(root) / f"{target}.json"


def _load_baseline(root: str, target: str) -> Optional[dict]:
    path = baseline_path(root, target)
    if not path.is_file():
        return None
    return json.loads(path.read_text(encoding="utf-8"))


def _atomic_write_text(path: Path, text: str) -> None:
    """Write text to path atomically: write to a sibling temp file, then
    os.replace() over the final path. A kill mid-write leaves the original
    file (or nothing, on first write) intact -- never a truncated/corrupt
    committed baseline."""
    tmp_path = path.with_name(f".{path.name}.tmp-{os.getpid()}")
    tmp_path.write_text(text, encoding="utf-8")
    os.replace(tmp_path, path)


def _write_baseline(root: str, target: str, payload: dict) -> Path:
    path = baseline_path(root, target)
    path.parent.mkdir(parents=True, exist_ok=True)
    _atomic_write_text(path, json.dumps(payload, indent=2, sort_keys=True) + "\n")
    return path


# ---------------------------------------------------------------------------
# Flash gate — bss shrink-only, flash_total growth capped at a threshold %.
# (Heap gate is phase B; it stays inert here while the baseline's `heap`
# block is null — this function never fails on heap fields.)
# ---------------------------------------------------------------------------

def _compare_flash(target: str, current: dict, baseline: dict, threshold_pct: float) -> bool:
    base_flash = baseline.get("flash", {}) or {}
    ok = True

    base_bss = base_flash.get("bss", 0)
    cur_bss = current["bss"]
    if cur_bss > base_bss:
        print(
            f"bbtool size[{target}]: FAIL bss grew {base_bss} -> {cur_bss}"
            " (bss is shrink-only)",
            file=sys.stderr,
        )
        ok = False
    else:
        print(f"bbtool size[{target}]: bss {cur_bss} (baseline {base_bss}) OK")

    base_total = base_flash.get("flash_total", 0)
    cur_total = current["flash_total"]
    delta = cur_total - base_total
    if base_total:
        pct = delta / base_total * 100.0
    else:
        pct = 0.0 if delta == 0 else float("inf")
    if delta > 0 and pct > threshold_pct:
        print(
            f"bbtool size[{target}]: FAIL flash_total grew {pct:.2f}%"
            f" (> {threshold_pct}% threshold): {base_total} -> {cur_total} (+{delta})",
            file=sys.stderr,
        )
        ok = False
    else:
        print(
            f"bbtool size[{target}]: flash_total {cur_total}"
            f" (baseline {base_total}, {pct:+.2f}%) OK"
        )

    print(f"bbtool size[{target}]: {'PASS' if ok else 'FAIL'}", file=(sys.stdout if ok else sys.stderr))
    return ok


# ---------------------------------------------------------------------------
# Live heap capture over HTTP — B1-719 phase B. Reads the bb_diag component's
# GET /api/diag/heap (platform/espidf/bb_diag/bb_diag_routes.c), a per-
# capability JSON object keyed by "internal"/"dma"/"spiram"/"exec"/"default"
# (only capabilities with nonzero total size are present), each with
# {free, allocated, largest_free_block, minimum_ever_free}. This is a
# device/fleet-run capability (needs a live board), distinct from the
# toolchain-only static flash measurement above.
# ---------------------------------------------------------------------------

_HEAP_HTTP_TIMEOUT = 5.0


def _fetch_json(url: str, timeout: float = _HEAP_HTTP_TIMEOUT) -> Tuple[Optional[dict], Optional[str]]:
    """GET url, parse JSON. Returns (parsed, None) on success, (None, error)
    on any urllib/timeout/JSON-decode failure -- never raises, so callers can
    surface a clean `bbtool size: error: ...` message instead of a
    traceback."""
    try:
        with urllib.request.urlopen(url, timeout=timeout) as resp:
            raw = resp.read()
    except urllib.error.URLError as exc:
        return None, f"request to {url} failed: {exc}"
    except TimeoutError as exc:
        return None, f"request to {url} timed out: {exc}"
    except OSError as exc:
        return None, f"request to {url} failed: {exc}"

    try:
        parsed = json.loads(raw)
    except json.JSONDecodeError as exc:
        return None, f"invalid JSON from {url}: {exc}"
    if not isinstance(parsed, dict):
        return None, f"unexpected JSON shape from {url} (expected an object)"
    return parsed, None


def _extract_heap(raw: dict) -> Tuple[Optional[dict], Optional[str]]:
    """Convert a raw /api/diag/heap response into the baseline `heap` block
    shape. `internal.minimum_ever_free` is the firmware's one watermark
    field (there is no separate peak-used counter), so `high_water` mirrors
    `min_free` -- both come from the same `minimum_ever_free` sample.
    `regions` captures every present capability (internal/dma/spiram/exec/
    default -- whichever the board actually has nonzero total size for)."""
    internal = raw.get("internal")
    if not isinstance(internal, dict):
        return None, "heap endpoint response missing 'internal' capability"
    min_free = internal.get("minimum_ever_free")
    if min_free is None:
        return None, "heap endpoint 'internal' entry missing 'minimum_ever_free'"

    regions: Dict[str, dict] = {}
    for name, entry in raw.items():
        if not isinstance(entry, dict):
            continue  # e.g. the optional top-level "integrity_ok" bool
        regions[name] = {
            "free": entry.get("free"),
            "min_free": entry.get("minimum_ever_free"),
            "largest_free_block": entry.get("largest_free_block"),
        }

    heap = {
        "min_free": min_free,
        "high_water": min_free,
        "free": internal.get("free"),
        "largest_free_block": internal.get("largest_free_block"),
        "regions": regions,
        "source": "http",
    }
    return heap, None


def _capture_heap_http(url: str) -> Tuple[Optional[dict], Optional[str]]:
    """Fetch + extract the heap block from <url>/api/diag/heap. Returns
    (heap, None) on success, (None, error) on any failure."""
    endpoint = f"{url.rstrip('/')}/api/diag/heap"
    raw, error = _fetch_json(endpoint)
    if error is not None:
        return None, error
    return _extract_heap(raw)


# ---------------------------------------------------------------------------
# Heap gate -- HIGHER-BETTER regression check (current < baseline = FAIL).
# This is the static-baseline-file equivalent of
# scripts/bbdevice/device/results.py's ResultSet.compare_baseline
# HIGHER_BETTER metric-direction logic (min_heap/heap_free_floor/...); it is
# reimplemented locally rather than imported because that comparator is
# shaped around a List[Result] fleet-run JSON (per-result named metrics),
# while bbtool's baseline file is a single nested per-target dict -- the two
# data shapes don't map cleanly onto one call, so this stays a small,
# self-contained mirror instead of a fragile cross-package (bbtool/bbdevice)
# import. Keep the two in sync by convention: HIGHER-BETTER, same-direction
# regression wording.
# ---------------------------------------------------------------------------

def _compare_heap(target: str, current: dict, baseline_heap: Optional[dict]) -> bool:
    if not baseline_heap or baseline_heap.get("min_free") is None:
        print(f"bbtool size[{target}]: heap check inert (no baseline heap yet)")
        return True

    ok = True

    def _check(label: str, cur_val, base_val) -> None:
        nonlocal ok
        if cur_val is None or base_val is None:
            return
        if cur_val < base_val:
            print(
                f"bbtool size[{target}]: FAIL heap.{label} regressed"
                f" {base_val} -> {cur_val} (higher-is-better)",
                file=sys.stderr,
            )
            ok = False
        else:
            print(f"bbtool size[{target}]: heap.{label} {cur_val} (baseline {base_val}) OK")

    _check("min_free", current.get("min_free"), baseline_heap.get("min_free"))
    _check("high_water", current.get("high_water"), baseline_heap.get("high_water"))

    base_regions = baseline_heap.get("regions") or {}
    cur_regions = current.get("regions") or {}
    for name in sorted(base_regions):
        base_entry = base_regions.get(name) or {}
        cur_entry = cur_regions.get(name) or {}
        _check(f"regions.{name}.min_free", cur_entry.get("min_free"), base_entry.get("min_free"))

    print(f"bbtool size[{target}]: heap {'PASS' if ok else 'FAIL'}", file=(sys.stdout if ok else sys.stderr))
    return ok


# ---------------------------------------------------------------------------
# Command entry point
# ---------------------------------------------------------------------------

def _resolve_root(args) -> str:
    root = getattr(args, "root", None) or getattr(args, "_root_abs", None) or os.getcwd()
    return os.path.abspath(root)


def run(args) -> int:
    build_dir = os.path.abspath(args.build_dir)
    elf_path = args.elf_path or os.path.join(build_dir, "firmware.elf")
    map_path = args.map_path or os.path.join(build_dir, "firmware.map")

    do_check = getattr(args, "check", False)
    do_update = getattr(args, "update_baseline", False)
    if do_check and do_update:
        print(
            "bbtool size: error: --check and --update-baseline are mutually exclusive",
            file=sys.stderr,
        )
        return 1

    result, error, warning = _measure(
        build_dir, elf_path, map_path, args.components, args.arch, args.size_tool_path,
    )
    if error is not None:
        print(f"bbtool size: error: {error}", file=sys.stderr)
        return 1
    if warning is not None:
        print(f"bbtool size: warning: {warning}", file=sys.stderr)

    if not do_check and not do_update:
        if getattr(args, "heap_from_http", None):
            print(
                "bbtool size: warning: --heap-from-http only applies with"
                " --update-baseline/--check; ignored in single-shot mode",
                file=sys.stderr,
            )
        print(json.dumps(result, sort_keys=True))
        return 0

    root = _resolve_root(args)
    target = getattr(args, "target", None) or Path(build_dir).name

    heap_from_http = getattr(args, "heap_from_http", None)

    if do_update:
        sha, snapshot_rel = _snapshot_config(build_dir, target, root)
        existing = _load_baseline(root, target) or {}
        heap = existing.get("heap") or dict(_HEAP_NULL)
        if heap_from_http:
            fetched_heap, heap_error = _capture_heap_http(heap_from_http)
            if heap_error is not None:
                print(f"bbtool size: error: {heap_error}", file=sys.stderr)
                return 1
            heap = fetched_heap
        payload = {
            "target": target,
            "arch": args.arch,
            "config": {
                "label": "default",
                "toolchain": "esp-idf",
                "sdkconfig_sha": sha,
                "snapshot": snapshot_rel,
            },
            "flash": {
                "text": result["text"],
                "data": result["data"],
                "bss": result["bss"],
                "flash_total": result["flash_total"],
                "components": result["components"],
            },
            "heap": heap,
        }
        path = _write_baseline(root, target, payload)
        print(f"bbtool size[{target}]: baseline updated -> {path}")
        return 0

    # do_check
    baseline = _load_baseline(root, target)
    if baseline is None:
        print(
            f"bbtool size: error: no baseline for target '{target}' at"
            f" {baseline_path(root, target)} (run --update-baseline first)",
            file=sys.stderr,
        )
        return 1
    threshold_pct = getattr(args, "flash_threshold_pct", 2.0)
    ok = _compare_flash(target, result, baseline, threshold_pct)

    if heap_from_http:
        current_heap, heap_error = _capture_heap_http(heap_from_http)
        if heap_error is not None:
            print(f"bbtool size: error: {heap_error}", file=sys.stderr)
            return 1
        heap_ok = _compare_heap(target, current_heap, baseline.get("heap"))
        ok = ok and heap_ok

    return 0 if ok else 1
