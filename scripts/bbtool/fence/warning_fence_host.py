"""warning_fence_host family — ratchet-fence over the host `-Wall` build's
genuine GCC diagnostics. See `fence/_gcc_warnings.py` for the shared
parsing + fail-closed live-report contract, and `warning_fence_firmware.py`
for the xtensa-side twin (split into two families deliberately: they are
enforced by two DIFFERENT CI jobs with two DIFFERENT toolchains available,
so a single combined family would hard-fail in whichever job's report
happens to be absent from that job's own checkout).
"""
from __future__ import annotations
import sys
from pathlib import Path
from typing import Set

from fence import _base
from fence._base import Marker
from fence._gcc_warnings import is_report_valid, normalize_quote_glyphs, scan_report

# `fence_cmd.py`'s `load_baseline(..., normalize_id=...)` hook (mirrors the
# `scan_is_trustworthy` hook below): folds a baseline entry's quote glyph to
# canonical at load time, so it compares canonical-vs-canonical against a
# freshly scanned (already-normalized, see `_gcc_warnings.parse_gcc_warnings`)
# marker regardless of which gcc/locale/older-fix-version seeded it.
normalize_id = normalize_quote_glyphs


def counts_by_bucket(markers: Set[Marker]) -> dict:
    return _base.counts_by_bucket(markers, bucket_fn=lambda t: f"-W{t}")


def _scan_host_warning(root: Path) -> Set[Marker]:
    return scan_report(root, "host")


def scan_all(root: str) -> Set[Marker]:
    return _base.scan_all(sys.modules[__name__], root)


def scan_is_trustworthy(root: str) -> bool:
    """`commands/fence_cmd.py`'s `--update-baseline` hook: False when the
    live report is missing/invalid (SKIP mode) -- an empty scan then must
    never be read as genuine shrinkage. See `_gcc_warnings.py`'s module
    docstring."""
    return is_report_valid(Path(root), "host")
