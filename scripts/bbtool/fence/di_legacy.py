"""di_legacy fence family — DI legacy ratchet-fence scanners.

Freezes breadboard's legacy dependency-injection glue surface (self-
registration macros, autoregister/auto-attach Kconfig options, pub-captive-
sink patterns, force-keep linker directives) as shrink-only. It scans
components/ + platform/ for a fixed set of legacy-glue markers; the `fence`
command diffs the current occurrence set against this family's committed
baseline (.baseline/bbtool/fence/di_legacy.json) and fails on any net-new
occurrence that isn't already in the baseline. Removals are never a
failure — they are reported as INFO "candidate to prune" so the baseline
can shrink over time.

Marker types scanned (see the `_scan_*` functions below for the concrete
regex per type):
  - BB_INIT_REGISTER family (BB_INIT_REGISTER[_EARLY|_PRE_HTTP][_N])
  - autoregister_kconfig / autoregister_usage  (*_AUTOREGISTER)
  - auto_attach_kconfig / auto_attach_usage    (*_AUTO_ATTACH)
  - pub_sink            (bb_pub_sink_t / bb_pub_add_sink)
  - display_force_keep  (BB_DISPLAY_AUTOREGISTER + bb_display_register__* + -u linker flags)
  - linker_force_register (bb_init_force_register[_early|_pre_http] CMake helper calls)

New composition (adding a component, a route, a satellite) never needs a new
occurrence of any of these — they are the legacy glue surface being phased
out, not the sanctioned extension point.
"""
from __future__ import annotations
import re
import sys
from pathlib import Path
from typing import Set

from fence import _base
from fence._base import Marker

_SCAN_ROOTS = ("components", "platform")

_SRC_GLOBS = ["**/*.c", "**/*.h", "**/*.cpp"]
_KCONFIG_GLOBS = ["**/Kconfig", "**/Kconfig.projbuild"]
_CMAKE_GLOBS = ["**/CMakeLists.txt"]


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


def counts_by_bucket(markers: Set[Marker]) -> dict:
    return _base.counts_by_bucket(markers, bucket_fn=_bucket_for)


# ---------------------------------------------------------------------------
# 1. BB_INIT_REGISTER family
# ---------------------------------------------------------------------------

_INIT_REGISTER_RE = re.compile(
    r'\b(BB_INIT_REGISTER(?:_EARLY|_PRE_HTTP)?(?:_N)?)\s*\(\s*'
    r'([A-Za-z_][A-Za-z0-9_]*)'
)


def _scan_init_register(root: Path) -> Set[Marker]:
    found: Set[Marker] = set()
    for path in _base.iter_files(root, _SCAN_ROOTS, ["**/*.c", "**/*.cpp"]):
        rel = _base.rel(root, path)
        for line in _base.read(path).splitlines():
            stripped = line.strip()
            if not stripped or stripped.startswith("#define") or _base.is_noise_line(stripped):
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

def _kconfig_symbols(root: Path, suffix: str, marker_type: str) -> Set[Marker]:
    found: Set[Marker] = set()
    pattern = re.compile(r'^[ \t]*config\s+(BB_[A-Za-z0-9_]*' + suffix + r')\s*$')
    for path in _base.iter_files(root, _SCAN_ROOTS, _KCONFIG_GLOBS):
        rel = _base.rel(root, path)
        for line in _base.read(path).splitlines():
            m = pattern.match(line)
            if m:
                found.add(Marker(marker_type, rel, m.group(1)))
    return found


def _config_usages(root: Path, suffix: str, marker_type: str) -> Set[Marker]:
    found: Set[Marker] = set()
    pattern = re.compile(r'\bCONFIG_(BB_[A-Za-z0-9_]*' + suffix + r')\b')
    for path in _base.iter_files(root, _SCAN_ROOTS, ["**/*.c", "**/*.h", "**/*.cpp"]):
        rel = _base.rel(root, path)
        for line in _base.read(path).splitlines():
            stripped = line.strip()
            if _base.is_noise_line(stripped):
                continue
            for m in pattern.finditer(line):
                found.add(Marker(marker_type, rel, m.group(1)))
    return found


def _scan_autoregister_kconfig(root: Path) -> Set[Marker]:
    return _kconfig_symbols(root, "_AUTOREGISTER", "autoregister_kconfig")


def _scan_autoregister_usage(root: Path) -> Set[Marker]:
    return _config_usages(root, "_AUTOREGISTER", "autoregister_usage")


def _scan_auto_attach_kconfig(root: Path) -> Set[Marker]:
    return _kconfig_symbols(root, "_AUTO_ATTACH", "auto_attach_kconfig")


def _scan_auto_attach_usage(root: Path) -> Set[Marker]:
    return _config_usages(root, "_AUTO_ATTACH", "auto_attach_usage")


# ---------------------------------------------------------------------------
# 6. pub-captive-sink markers: bb_pub_sink_t / bb_pub_add_sink
# ---------------------------------------------------------------------------

_PUB_SINK_RE = re.compile(r'\b(bb_pub_sink_t|bb_pub_add_sink)\b')


def _scan_pub_sink(root: Path) -> Set[Marker]:
    found: Set[Marker] = set()
    for path in _base.iter_files(root, _SCAN_ROOTS, _SRC_GLOBS):
        rel = _base.rel(root, path)
        for line in _base.read(path).splitlines():
            stripped = line.strip()
            if _base.is_noise_line(stripped):
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
    for path in _base.iter_files(root, _SCAN_ROOTS, ["**/*.c", "**/*.cpp"]):
        rel = _base.rel(root, path)
        for line in _base.read(path).splitlines():
            stripped = line.strip()
            if stripped.startswith("#define") or _base.is_noise_line(stripped):
                continue
            m = _DISPLAY_AUTOREGISTER_RE.search(stripped)
            if m:
                found.add(Marker("display_force_keep", rel, f"macro:{m.group(1)}"))
    for path in _base.iter_files(root, _SCAN_ROOTS, _CMAKE_GLOBS):
        rel = _base.rel(root, path)
        for line in _base.read(path).splitlines():
            stripped = line.strip()
            if _base.is_cmake_noise_line(stripped):
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
    for path in _base.iter_files(root, _SCAN_ROOTS, _CMAKE_GLOBS):
        rel = _base.rel(root, path)
        for line in _base.read(path).splitlines():
            stripped = line.strip()
            if _base.is_cmake_noise_line(stripped):
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
# Identity override — pub_sink's `id` is not itself a unique symbol name
# (the literal string "bb_pub_sink_t" / "bb_pub_add_sink" at every call
# site), so two different files both get the exact same id. For that type,
# fall back to the enclosing directory ("owning component") as a stand-in
# symbol key so different call sites don't collide under identity.
# ---------------------------------------------------------------------------

_PATH_INSENSITIVE_ID_TYPES = {"pub_sink"}


def _component_of(path: str) -> str:
    """Best-effort 'owning component' name for a marker path: the parent
    directory name. Stable across a pure filename rename within the same
    component directory; changes only if the component directory itself is
    renamed/moved — that case is treated as a legitimate baseline update,
    not a spurious rename false-positive."""
    return Path(path).parent.name


def identity(m: Marker):
    """Ratchet-diff identity key: (marker_type, symbol_identifier) — NEVER
    the file path. This is what makes the fence rename-stable: moving a
    marker to a new file (same symbol, same type) is a no-op diff, not a
    remove+add.

    If two genuinely-distinct markers ever share an identity (e.g. the same
    registrant symbol registered twice under an #ifdef/#else), this
    intentionally COLLAPSES them into one entry rather than risk a
    spurious-new failure — under-keying is the safe direction for a ratchet
    fence."""
    if m.type in _PATH_INSENSITIVE_ID_TYPES:
        return (m.type, f"{_component_of(m.path)}:{m.id}")
    return (m.type, m.id)


def scan_all(root: str) -> Set[Marker]:
    return _base.scan_all(sys.modules[__name__], root)
