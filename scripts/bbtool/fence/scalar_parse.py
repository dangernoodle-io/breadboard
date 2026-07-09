"""scalar_parse fence family — ratchet-fence for hand-rolled reimplementations
of the already-extracted `bb_scalar` strict scalar parsers.

`bb_scalar` (components/bb_scalar) extracted a small, previously-duplicated
idiom into a portable, host-testable component: strict scalar parsing
(`bb_scalar_parse_bool`/`bb_scalar_parse_uint`, mirroring bb_http_server's
`bb_url_parse_bool`/`bb_url_parse_uint`, whose migration is deferred — see
bb_scalar.h). This fence freezes the CURRENT set of hand-rolled
reimplementations as a draining baseline: no *new* one may appear outside
the canonical component, but existing sites are grandfathered until
migrated onto `bb_scalar`.

One family per shared helper (see also `fence/clamp.py`) — this is the
natural "family = module" use of the generic fence engine (`fence/_base.py`);
it makes per-helper lockdown/draindown and a fresh `--seed` per helper
clean, and keeps adding a future helper's fence a turnkey new module rather
than a combined-family rewrite.

Marker type scanned: `scalar_parse` — a DEFINITION of
bb_url_parse_bool/bb_url_parse_uint (the known bb_scalar duplicate) outside
bb_scalar itself. Symbol-keyed: id is the function name itself.

ACCEPTED LIMITATION: this only catches reintroduction of these two named
symbols, not arbitrary hand-rolled inline parsing that duplicates their
behavior without reusing the name — a fuller "parses a bool/uint from a
string" behavioral scan would need real semantic analysis this stdlib-only
tool doesn't have. Symbol-keyed is the pragmatic middle ground: it still
catches the concrete, known duplicate (bb_http_server's originals) and any
copy-paste of them elsewhere.
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

# ---------------------------------------------------------------------------
# Marker-type -> reporting bucket
# ---------------------------------------------------------------------------

_BUCKETS = {
    "scalar_parse": "hand-rolled scalar_parse",
}


def _bucket_for(marker_type: str) -> str:
    return _BUCKETS.get(marker_type, marker_type)


def counts_by_bucket(markers: Set[Marker]) -> dict:
    return _base.counts_by_bucket(markers, bucket_fn=_bucket_for)


# ---------------------------------------------------------------------------
# scalar_parse — definitions of the known legacy parsers
# bb_url_parse_bool / bb_url_parse_uint outside bb_scalar/its canonical host
# impl.
# ---------------------------------------------------------------------------

_SCALAR_PARSE_NAMES = ("bb_url_parse_bool", "bb_url_parse_uint")
# MULTILINE: `^` anchors each candidate to the start of its own source
# line (leading whitespace allowed) rather than requiring a pre-stripped
# single line — needed so the forward balanced-paren scan below can walk
# straight through the rest of `text` from the match position.
_SCALAR_PARSE_DEF_RE = re.compile(
    r'(?m)^[ \t]*(?:static\s+)?bool\s+(' + "|".join(_SCALAR_PARSE_NAMES) + r')\s*\('
)
_SCALAR_PARSE_EXCLUDE_PREFIXES = ("components/bb_scalar/", "platform/host/bb_scalar/")


def _sig_terminator(text: str, open_paren_idx: int):
    """Scan forward from the `(` at `open_paren_idx`, tracking paren depth
    across line breaks, to the matching close paren, then return the first
    non-whitespace character after it: `;` for a declaration (a wrapped,
    multi-line prototype like `bool f(a,\\n b);` still ends in `;` after
    the closing paren, however many lines the parameter list spans), `{`
    for a definition, or `None` if neither is found before EOF (treated as
    ambiguous — see `_scan_scalar_parse`)."""
    depth = 0
    i = open_paren_idx
    n = len(text)
    while i < n:
        c = text[i]
        if c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                j = i + 1
                while j < n and text[j].isspace():
                    j += 1
                return text[j] if j < n else None
        i += 1
    return None


def _scan_scalar_parse(root: Path) -> Set[Marker]:
    found: Set[Marker] = set()
    for path in _base.iter_files(root, _SCAN_ROOTS, _SRC_GLOBS):
        rel = _base.rel(root, path)
        if rel.startswith(_SCALAR_PARSE_EXCLUDE_PREFIXES):
            continue
        text = _base.read(path)
        lines = text.splitlines()
        for m in _SCALAR_PARSE_DEF_RE.finditer(text):
            line_idx = text.count("\n", 0, m.start())
            if _base.is_noise_line(lines[line_idx].strip()):
                continue
            open_paren_idx = m.end() - 1  # the regex ends with a literal '('
            terminator = _sig_terminator(text, open_paren_idx)
            if terminator != "{":
                # ';' -> declaration (header prototype, possibly wrapped
                # across multiple lines); None -> ambiguous/EOF — either
                # way, not a reimplementation to flag.
                continue
            found.add(Marker("scalar_parse", rel, m.group(1)))
    return found


def scan_all(root: str) -> Set[Marker]:
    return _base.scan_all(sys.modules[__name__], root)
