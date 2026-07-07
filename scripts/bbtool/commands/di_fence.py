"""di-fence command — DI legacy ratchet-fence lint.

Freezes breadboard's legacy dependency-injection glue surface (self-
registration macros, autoregister/auto-attach Kconfig options, pub-captive-
sink patterns, force-keep linker directives) as shrink-only. It scans
components/ + platform/ for a fixed set of legacy-glue markers, compares the
current occurrence set to a committed baseline
(scripts/bbtool/di_legacy_baseline.json), and FAILS if any net-new occurrence
appears that isn't already in the baseline. Removals are never a failure —
they are reported as INFO "candidate to prune" so the baseline can shrink
over time.

Marker types scanned (see scan_all() below for the concrete regex per type):
  - BB_INIT_REGISTER family (BB_INIT_REGISTER[_EARLY|_PRE_HTTP][_N])
  - autoregister_kconfig / autoregister_usage  (*_AUTOREGISTER)
  - auto_attach_kconfig / auto_attach_usage    (*_AUTO_ATTACH)
  - pub_sink            (bb_pub_sink_t / bb_pub_add_sink)
  - display_force_keep  (BB_DISPLAY_AUTOREGISTER + bb_display_register__* + -u linker flags)
  - linker_force_register (bb_init_force_register[_early|_pre_http] CMake helper calls)

New composition (adding a component, a route, a satellite) never needs a new
occurrence of any of these — they are the legacy glue surface being phased
out, not the sanctioned extension point. Legitimate conversions/relocations
of *existing* markers move the baseline forward via --update-baseline.
"""
from __future__ import annotations
import argparse
import json
import os
import re
import sys
from pathlib import Path
from typing import List, NamedTuple, Set

NAME = "di-fence"
HELP = "DI legacy ratchet-fence lint (freeze self-registration glue surface shrink-only)"

_BASELINE_REL_PATH = ("scripts", "bbtool", "di_legacy_baseline.json")

_SCAN_ROOTS = ("components", "platform")
_EXCLUDE_DIRS = {".pio", ".claude", "build", "managed_components", "test", "tests"}

_SRC_GLOBS = ["**/*.c", "**/*.h", "**/*.cpp"]
_KCONFIG_GLOBS = ["**/Kconfig", "**/Kconfig.projbuild"]
_CMAKE_GLOBS = ["**/CMakeLists.txt"]


class Marker(NamedTuple):
    type: str
    path: str   # POSIX-style, relative to repo root
    id: str


# ---------------------------------------------------------------------------
# Marker-type -> reporting bucket (for the human-readable per-family summary)
# ---------------------------------------------------------------------------

_BUCKETS = {
    "BB_INIT_REGISTER": "BB_INIT_REGISTER family",
    "BB_INIT_REGISTER_N": "BB_INIT_REGISTER family",
    "BB_INIT_REGISTER_EARLY": "BB_INIT_REGISTER family",
    "BB_INIT_REGISTER_EARLY_N": "BB_INIT_REGISTER family",
    "BB_INIT_REGISTER_PRE_HTTP": "BB_INIT_REGISTER family",
    "BB_INIT_REGISTER_PRE_HTTP_N": "BB_INIT_REGISTER family",
    "autoregister_kconfig": "AUTOREGISTER",
    "autoregister_usage": "AUTOREGISTER",
    "auto_attach_kconfig": "AUTO_ATTACH",
    "auto_attach_usage": "AUTO_ATTACH",
    "pub_sink": "pub-sink",
    "display_force_keep": "DISPLAY_AUTOREGISTER/force-keep",
    "linker_force_register": "linker force-register",
}


def _bucket_for(marker_type: str) -> str:
    return _BUCKETS.get(marker_type, marker_type)


# ---------------------------------------------------------------------------
# File walking
# ---------------------------------------------------------------------------

def _iter_files(root: Path, globs: List[str]) -> List[Path]:
    out: List[Path] = []
    for scan_root_name in _SCAN_ROOTS:
        scan_root = root / scan_root_name
        if not scan_root.is_dir():
            continue
        for pattern in globs:
            for path in scan_root.glob(pattern):
                if not path.is_file():
                    continue
                parts = path.relative_to(root).parts
                if any(p in _EXCLUDE_DIRS for p in parts):
                    continue
                out.append(path)
    return sorted(set(out))


def _read(path: Path) -> str:
    with open(path, encoding="utf-8", errors="replace") as fh:
        return fh.read()


def _rel(root: Path, path: Path) -> str:
    return path.relative_to(root).as_posix()


def _is_noise_line(stripped: str) -> bool:
    """Cheap (non-tokenizing) comment-line filter: skip lines that are
    entirely a // comment or a /* */ continuation (leading '*'). Deliberately
    conservative — it does not strip trailing // comments or handle every
    block-comment shape, since over-matching (treating a commented-out
    reference as a real occurrence) is the safe failure direction for a
    ratchet fence: worst case is a baseline entry that never needs pruning,
    not a missed regression."""
    return stripped.startswith("//") or stripped.startswith("*") or stripped.startswith("/*")


def _is_cmake_noise_line(stripped: str) -> bool:
    """CMakeLists comment-line filter: skip lines that are entirely a `#`
    comment. CMake syntax has no block-comment leading-char convention akin
    to C's ` * `, so this is intentionally simpler than _is_noise_line —
    used only by the two CMakeLists-scanning passes (display force-keep
    linker flags, bb_init_force_register* helper calls) so a commented-out
    `# bb_init_force_register(...)` line is not counted as a live marker."""
    return stripped.startswith("#")


# ---------------------------------------------------------------------------
# 1. BB_INIT_REGISTER family
# ---------------------------------------------------------------------------

_INIT_REGISTER_RE = re.compile(
    r'\b(BB_INIT_REGISTER(?:_EARLY|_PRE_HTTP)?(?:_N)?)\s*\(\s*'
    r'([A-Za-z_][A-Za-z0-9_]*)'
)


def _scan_init_register(root: Path) -> Set[Marker]:
    found: Set[Marker] = set()
    for path in _iter_files(root, ["**/*.c", "**/*.cpp"]):
        rel = _rel(root, path)
        for line in _read(path).splitlines():
            stripped = line.strip()
            if not stripped or stripped.startswith("#define") or _is_noise_line(stripped):
                continue
            m = _INIT_REGISTER_RE.search(stripped)
            if not m:
                continue
            variant, ident = m.group(1), m.group(2)
            found.add(Marker(variant, rel, ident))
    return found


# ---------------------------------------------------------------------------
# 2/3/4/5. *_AUTOREGISTER and *_AUTO_ATTACH — Kconfig defs + CONFIG_ usages
# ---------------------------------------------------------------------------

def _scan_kconfig_symbols(root: Path, suffix: str, marker_type: str) -> Set[Marker]:
    found: Set[Marker] = set()
    pattern = re.compile(r'^config\s+(BB_[A-Za-z0-9_]*' + suffix + r')\s*$')
    for path in _iter_files(root, _KCONFIG_GLOBS):
        rel = _rel(root, path)
        for line in _read(path).splitlines():
            m = pattern.match(line)
            if m:
                found.add(Marker(marker_type, rel, m.group(1)))
    return found


def _scan_config_usages(root: Path, suffix: str, marker_type: str) -> Set[Marker]:
    found: Set[Marker] = set()
    pattern = re.compile(r'\bCONFIG_(BB_[A-Za-z0-9_]*' + suffix + r')\b')
    for path in _iter_files(root, ["**/*.c", "**/*.h", "**/*.cpp"]):
        rel = _rel(root, path)
        for line in _read(path).splitlines():
            stripped = line.strip()
            if _is_noise_line(stripped):
                continue
            for m in pattern.finditer(line):
                found.add(Marker(marker_type, rel, m.group(1)))
    return found


# ---------------------------------------------------------------------------
# 6. pub-captive-sink markers: bb_pub_sink_t / bb_pub_add_sink
# ---------------------------------------------------------------------------

_PUB_SINK_RE = re.compile(r'\b(bb_pub_sink_t|bb_pub_add_sink)\b')


def _scan_pub_sink(root: Path) -> Set[Marker]:
    found: Set[Marker] = set()
    for path in _iter_files(root, _SRC_GLOBS):
        rel = _rel(root, path)
        for line in _read(path).splitlines():
            stripped = line.strip()
            if _is_noise_line(stripped):
                continue
            for m in _PUB_SINK_RE.finditer(line):
                found.add(Marker("pub_sink", rel, m.group(1)))
    return found


# ---------------------------------------------------------------------------
# 7. Display force-keep: BB_DISPLAY_AUTOREGISTER(...) invocations,
#    bb_display_register__<chip> symbol refs, and their -u linker flags
# ---------------------------------------------------------------------------

_DISPLAY_AUTOREGISTER_RE = re.compile(
    r'\bBB_DISPLAY_AUTOREGISTER\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)'
)
_DISPLAY_FORCE_KEEP_LINK_RE = re.compile(
    r'-u\s+bb_display_register__([A-Za-z0-9_]+)'
)

# ---------------------------------------------------------------------------
# KNOWN GAP: Arduino display self-registration is NOT scanned.
#
# This fence covers ESP-IDF (BB_DISPLAY_AUTOREGISTER macro + -u linker
# force-keep) and host builds only. The Arduino backend
# (platform/arduino/bb_display_*) self-registers the same display-backend
# family via a constructor-attribute function DEFINITION —
# `void bb_display_register__<chip>(void) __attribute__((constructor));`
# followed by the definition — which this scan deliberately does NOT match.
# Arduino is behind ESP-IDF on platform parity and not under active
# development right now, so we are not investing scanner effort there yet.
# This is an intentional, documented gap, not an oversight: today there are
# 4 live Arduino constructor registrations (bb_display_ssd1306,
# bb_display_ili9341, bb_display_st77xx x2) with ZERO baseline coverage.
# Revisit this scanner when Arduino display work resumes.
# ---------------------------------------------------------------------------


def _scan_display_force_keep(root: Path) -> Set[Marker]:
    found: Set[Marker] = set()
    for path in _iter_files(root, ["**/*.c", "**/*.cpp"]):
        rel = _rel(root, path)
        for line in _read(path).splitlines():
            stripped = line.strip()
            if stripped.startswith("#define") or _is_noise_line(stripped):
                continue
            m = _DISPLAY_AUTOREGISTER_RE.search(stripped)
            if m:
                found.add(Marker("display_force_keep", rel, f"macro:{m.group(1)}"))
    for path in _iter_files(root, _CMAKE_GLOBS):
        rel = _rel(root, path)
        for line in _read(path).splitlines():
            stripped = line.strip()
            if _is_cmake_noise_line(stripped):
                continue
            m = _DISPLAY_FORCE_KEEP_LINK_RE.search(line)
            if m:
                found.add(Marker("display_force_keep", rel, f"linker:{m.group(1)}"))
    return found


# ---------------------------------------------------------------------------
# 8. bb_init_force_register[_early|_pre_http] CMake helper call sites
# ---------------------------------------------------------------------------

# The helper's first positional arg is always the literal CMake variable
# ${COMPONENT_LIB} (every call site in this repo follows the documented
# `bb_init_force_register(${COMPONENT_LIB} bb_<name>)` convention, see
# CLAUDE.md's Registry section) — matched literally rather than captured,
# since it is never anything else in practice.
_FORCE_REGISTER_HELPER_RE = re.compile(
    r'\bbb_init_force_register(_early|_pre_http)?\s*\(\s*\$\{COMPONENT_LIB\}\s*,?\s*'
    r'([A-Za-z_][A-Za-z0-9_]*)\s*\)'
)
_RAW_FORCE_KEEP_LINK_RE = re.compile(
    r'-u\s+bb_init_register(?:_early|_pre_http)?__([A-Za-z0-9_]+)'
)


def _scan_linker_force_register(root: Path) -> Set[Marker]:
    found: Set[Marker] = set()
    for path in _iter_files(root, _CMAKE_GLOBS):
        rel = _rel(root, path)
        for line in _read(path).splitlines():
            stripped = line.strip()
            if _is_cmake_noise_line(stripped):
                continue
            m = _FORCE_REGISTER_HELPER_RE.search(line)
            if m:
                tier = (m.group(1) or "").lstrip("_") or "regular"
                found.add(Marker("linker_force_register", rel, f"{tier}:{m.group(2)}"))
            m2 = _RAW_FORCE_KEEP_LINK_RE.search(line)
            if m2:
                found.add(Marker("linker_force_register", rel, f"raw:{m2.group(1)}"))
    return found


# ---------------------------------------------------------------------------
# Aggregate scan
# ---------------------------------------------------------------------------

def scan_all(root: str) -> Set[Marker]:
    root_p = Path(root)
    found: Set[Marker] = set()
    found |= _scan_init_register(root_p)
    found |= _scan_kconfig_symbols(root_p, "_AUTOREGISTER", "autoregister_kconfig")
    found |= _scan_config_usages(root_p, "_AUTOREGISTER", "autoregister_usage")
    found |= _scan_kconfig_symbols(root_p, "_AUTO_ATTACH", "auto_attach_kconfig")
    found |= _scan_config_usages(root_p, "_AUTO_ATTACH", "auto_attach_usage")
    found |= _scan_pub_sink(root_p)
    found |= _scan_display_force_keep(root_p)
    found |= _scan_linker_force_register(root_p)
    return found


# ---------------------------------------------------------------------------
# Baseline load/save
# ---------------------------------------------------------------------------

def baseline_path(root: str) -> Path:
    return Path(root).joinpath(*_BASELINE_REL_PATH)


def load_baseline(root: str) -> Set[Marker]:
    path = baseline_path(root)
    if not path.is_file():
        return set()
    data = json.loads(path.read_text(encoding="utf-8"))
    return {Marker(e["type"], e["path"], e["id"]) for e in data.get("entries", [])}


def write_baseline(root: str, markers: Set[Marker]) -> Path:
    path = baseline_path(root)
    entries = [
        {"type": m.type, "path": m.path, "id": m.id}
        for m in sorted(markers, key=lambda m: (m.type, m.path, m.id))
    ]
    payload = {"entries": entries}
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return path


# ---------------------------------------------------------------------------
# Diff + report
# ---------------------------------------------------------------------------

# Marker types whose `id` field is not itself a unique symbol name — e.g.
# pub_sink's id is the literal string "bb_pub_sink_t" / "bb_pub_add_sink" at
# every call site, so two different files both get the exact same id. For
# these, fall back to the enclosing directory ("owning component") as a
# stand-in symbol key so different call sites don't collide under identity.
_PATH_INSENSITIVE_ID_TYPES = {"pub_sink"}


def _component_of(path: str) -> str:
    """Best-effort 'owning component' name for a marker path: the parent
    directory name. Stable across a pure filename rename within the same
    component directory; changes only if the component directory itself is
    renamed/moved — that case is treated as a legitimate baseline update
    (--update-baseline), not a spurious rename false-positive."""
    return Path(path).parent.name


def _identity(m: Marker):
    """Ratchet-diff identity key: (marker_type, symbol_identifier) — NEVER
    the file path. This is what makes the fence rename-stable: moving a
    marker to a new file (same symbol, same type) is a no-op diff, not a
    remove+add. Path is retained on Marker only as informational metadata
    for the human-readable report (see diff() below) and the baseline JSON.

    If two genuinely-distinct markers ever share an identity (e.g. the same
    registrant symbol registered twice under an #ifdef/#else, as
    bb_diag_panic.c does for BB_INIT_REGISTER_EARLY(bb_diag_panic, ...)),
    this intentionally COLLAPSES them into one entry rather than risk a
    spurious-new failure — under-keying is the safe direction for a ratchet
    fence (worst case: one fewer baseline entry to prune later)."""
    if m.type in _PATH_INSENSITIVE_ID_TYPES:
        return (m.type, f"{_component_of(m.path)}:{m.id}")
    return (m.type, m.id)


def diff(current: Set[Marker], baseline: Set[Marker]):
    """Returns (new_markers, removed_markers) — both sorted lists.

    Comparison is by _identity(), not the full (type, path, id) tuple, so a
    pure file rename of an existing marker is never reported as a
    remove+add. `new`/`removed` still carry full Marker instances (path
    included) for the human-readable report — one representative per
    identity (current's own entry for `new`; baseline's for `removed`)."""
    cur_by_identity: dict = {}
    for m in current:
        cur_by_identity.setdefault(_identity(m), m)
    base_by_identity: dict = {}
    for m in baseline:
        base_by_identity.setdefault(_identity(m), m)

    new_ids = set(cur_by_identity) - set(base_by_identity)
    removed_ids = set(base_by_identity) - set(cur_by_identity)

    new = sorted((cur_by_identity[i] for i in new_ids), key=lambda m: (m.type, m.path, m.id))
    removed = sorted((base_by_identity[i] for i in removed_ids), key=lambda m: (m.type, m.path, m.id))
    return new, removed


def counts_by_bucket(markers: Set[Marker]) -> dict:
    out: dict = {}
    for m in markers:
        bucket = _bucket_for(m.type)
        out[bucket] = out.get(bucket, 0) + 1
    return out


# ---------------------------------------------------------------------------
# Command interface
# ---------------------------------------------------------------------------

def add_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument(
        "--root",
        default=os.getcwd(),
        help="repository root (default: from top-level --root or cwd)",
    )
    parser.add_argument(
        "--update-baseline",
        action="store_true",
        help="regenerate scripts/bbtool/di_legacy_baseline.json from the current"
             " scan and exit 0 (use for legitimate conversions/relocations of"
             " existing legacy markers)",
    )


def run(args: argparse.Namespace) -> int:
    root = getattr(args, "root", None) or getattr(args, "_root_abs", None) or os.getcwd()
    root = os.path.abspath(root)

    current = scan_all(root)

    if getattr(args, "update_baseline", False):
        path = write_baseline(root, current)
        print(f"bbtool di-fence: baseline updated ({len(current)} entries) -> {path}")
        return 0

    baseline = load_baseline(root)
    new, removed = diff(current, baseline)

    for m in removed:
        print(f"INFO [di-fence]: candidate to prune from baseline: {m.path}:{m.id} ({m.type})")

    if new:
        for m in new:
            print(
                f"ERROR [di-fence]: new legacy DI marker added: {m.path}:{m.id} ({m.type})"
                " — the legacy surface is frozen shrink-only; compose instead,"
                " or if this is a conversion, run --update-baseline",
                file=sys.stderr,
            )
        print(
            f"bbtool di-fence: {len(new)} new legacy DI marker(s) — FAIL",
            file=sys.stderr,
        )
        return 1

    print(f"bbtool di-fence: {len(current)} marker(s), 0 new — PASS")
    return 0


def register(api) -> None:
    api.add_command(NAME, sys.modules[__name__])
