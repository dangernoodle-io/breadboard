"""Shared plumbing for the `warning_fence_host` / `warning_fence_firmware`
families (B1-1420 PR2): GCC `-Wall` diagnostic parsing plus the
live-build-artifact report contract that `scripts/warn_scan.sh` writes and
`.github/workflows/build.yml` invokes.

Leading underscore -- like `_base.py` -- so `fence/__init__.py`'s
auto-discovery (every non-underscore module under `fence/` becomes a
family) never treats this shared helper as its own family.

Report contract (scripts/warn_scan.sh is the ONLY writer):
    line 1:  "# bb-warn-scan v1 target=<host|firmware> build_exit=<N>"
    lines 2+: the `-Wall -Wno-error` build's `warning:`/`In function`/
              `At top level` diagnostic lines (relative-POSIX paths), or
              nothing at all when there are genuinely zero warnings.

Fail-closed contract (the whole point of this rewrite -- the original
snapshot design let the fence go green forever the moment nobody re-ran the
generator): a report that is MISSING, empty, unreadable, whose header line
doesn't parse, or whose embedded `build_exit` is non-zero (the capture
build itself broke, so its diagnostic output can't be trusted as complete)
is NEVER treated as "zero warnings" — absence of evidence is not evidence
of a clean build.

Enforcement is gated on `BB_WARNING_FENCE_STRICT=1`, NOT the generic `CI`
env var GitHub Actions sets on every job. Deliberately narrower: `CI` is
also `true` in this repo's `check` job, which runs bare `make check` on
ubuntu-latest with no PlatformIO and no build step at all (see
`.github/workflows/build.yml`) -- it can never have a fresh report, so
gating on plain `CI` would hard-fail that job on every single run, not just
when a warning regresses. Only the two jobs that actually run
`scripts/warn_scan.sh` (see below) export the strict flag right before
invoking this family's fence check, so they alone are the enforcing
authority; every other context (the `check` job, a dev machine, `make
check`/`make fence`'s generic default sweep) behaves like local dev: a
loud, explicit SKIP, never a silent pass and never a spurious hard fail.

  - `BB_WARNING_FENCE_STRICT=1`: any of the above HARD-FAILS the scan
    (`WarningReportInvalidError`, wrapped by `fence/_base.py`'s
    `ScannerError` like any other broken scanner). Set by
    `.github/workflows/build.yml`'s `test` job (host -- the job that
    already builds natively for coverage) and the `smoke` job's `esp32`
    matrix leg (firmware), each immediately after running
    `scripts/warn_scan.sh <target>` in the SAME step/job that produced the
    report, so a new warning fails the build where it's produced.
  - Otherwise (unset -- the `check` job, `make test-py`, a dev machine that
    may not have PlatformIO or a genuine-GNU gcc at all): a loud, explicit
    SKIP to stderr, scanning as zero markers -- `make check`/`make fence`
    must stay usable without a toolchain, but must never look like it
    validated something it didn't.

`--update-baseline` guard (B1-1420 PR2 review): `commands/fence_cmd.py`'s
generic `diff()`/prune path assumes "current scan found fewer markers than
baseline" means genuine shrinkage -- true for every filesystem-walking
family, but WRONG here, since a SKIP-mode scan legitimately returns an
empty set for "not scanned here", indistinguishable by `diff()` alone from
"genuinely zero warnings". `is_report_valid` above backs each family's
`scan_is_trustworthy(root)` hook, which `_update_baseline` consults BEFORE
pruning -- an untrustworthy (SKIP-mode) scan is a no-op, never a prune,
even though it would still safely PASS a plain `fence` check (an empty
`current` can never produce a `new` marker either).
"""
from __future__ import annotations
import os
import re
import sys
from pathlib import Path
from typing import Optional, Set

from fence._base import Marker

REPORTS_DIRNAME = "warning_fence_reports"

_ENCLOSING_FN_RE = re.compile(r"^(?P<file>[^:]+): In function '(?P<fn>[^']+)':$")
_TOP_LEVEL_RE = re.compile(r"^(?P<file>[^:]+): At top level:$")
_DIAG_RE = re.compile(r"^(?P<path>[^:]+):(?P<line>\d+):(?P<col>\d+): warning: (?P<msg>.*)$")
_FLAG_RE = re.compile(r"\s\[-W(?P<flag>[A-Za-z0-9-]+=?)\]\s*$")
_HEADER_RE = re.compile(r"^# bb-warn-scan v1 target=(?P<target>\w+) build_exit=(?P<exit>\d+)\s*$")

# Quote style is never semantic in a gcc diagnostic -- it's delimiter
# decoration around a symbol or token, and it varies by gcc version, build,
# and message-catalog/locale state: curly Unicode quotes (U+2018/U+2019,
# `%qs`-style under a UTF-8 locale/catalog), or straight ASCII single (')
# or double (") quotes depending on the specific diagnostic's format
# string and gcc build. Two diagnostics differing only in quote glyph are
# the same diagnostic, so every glyph form is folded to a single canonical
# straight single quote before keying -- this is the only defense (no
# capture-time locale-forcing: that changes diagnostic WORDING, not just
# quoting -- e.g. LC_ALL=C renames `strncpy` to `__builtin_strncpy` in the
# stringop-truncation warning by bypassing gcc's gettext catalog, which is
# worse than the bug it would "fix"). The symbol name inside the quotes is
# left untouched -- only the glyph itself is normalized, never stripped.
#
# A key-derivation function must be applied everywhere a key is derived,
# including from stored data -- this is exported (no leading underscore,
# unlike the rest of this module's internals) and re-exported as
# `normalize_id` by both warning_fence_host.py and warning_fence_firmware.py,
# which `fence/_base.py::load_baseline`'s optional `normalize_id` hook
# applies to each baseline entry's `id` AT LOAD TIME -- never by rewriting
# the committed JSON on read. That makes a `parse_gcc_warnings` scan (this
# module) and a loaded baseline entry (however it happened to be seeded --
# a different gcc, a different locale, an older version of this fix)
# canonical-vs-canonical at comparison time, with no baseline edit needed:
# an already-canonical entry is a no-op, and a stale-glyph entry converges
# to canonical the next time `--update-baseline` legitimately prunes
# something (surviving entries are re-written from the now-normalized
# in-memory set), with zero migration step required.
_QUOTE_NORMALIZE = str.maketrans({
    "‘": "'",  # LEFT SINGLE QUOTATION MARK
    "’": "'",  # RIGHT SINGLE QUOTATION MARK
    "“": "'",  # LEFT DOUBLE QUOTATION MARK
    "”": "'",  # RIGHT DOUBLE QUOTATION MARK
    '"': "'",  # ASCII double quote
})


def normalize_quote_glyphs(text: str) -> str:
    return text.translate(_QUOTE_NORMALIZE)


class WarningReportInvalidError(RuntimeError):
    """CI-only hard failure: see module docstring's fail-closed contract."""


def report_path(root: Path, target: str) -> Path:
    return root / ".baseline" / "bbtool" / "fence" / REPORTS_DIRNAME / f"{target}.log"


def is_strict() -> bool:
    return os.environ.get("BB_WARNING_FENCE_STRICT", "").strip() == "1"


def parse_gcc_warnings(text: str) -> Set[Marker]:
    """Parse the diagnostic lines of a captured `-Wall -Wno-error` build log
    (already header-stripped -- see `scan_report` below) into `Set[Marker]`.
    Every other line (build noise, `note:`, code snippets) matches neither
    regex below and is silently skipped.
    """
    found: Set[Marker] = set()
    current_file: Optional[str] = None
    current_fn: Optional[str] = None

    for raw_line in text.splitlines():
        # Normalize the whole line up front, not just the extracted key
        # pieces -- gcc's own "In function '...'"/"'...' " quoting (parsed
        # by _ENCLOSING_FN_RE below) is subject to the same locale-driven
        # curly-vs-ASCII quote variance as the diagnostic message text, so
        # normalizing only after extraction would still leave that regex
        # failing to match a curly-quoted line.
        line = normalize_quote_glyphs(raw_line)
        m = _ENCLOSING_FN_RE.match(line)
        if m:
            current_file = m.group("file")
            current_fn = m.group("fn")
            continue
        m = _TOP_LEVEL_RE.match(line)
        if m:
            current_file = m.group("file")
            current_fn = None
            continue
        m = _DIAG_RE.match(line)
        if not m:
            continue
        path, msg = m.group("path"), m.group("msg")
        if path != current_file:
            # A file transition with no preceding context header (GCC omits
            # one for some file-scope diagnostics) -- reset scope tracking
            # rather than risk misattributing to a stale prior file's fn.
            current_file = path
            current_fn = None
        flag_m = _FLAG_RE.search(msg)
        flag = flag_m.group("flag") if flag_m else "unflagged"
        clean_msg = _FLAG_RE.sub("", msg).strip()
        ident = f"{current_fn}: {clean_msg}" if current_fn else clean_msg
        found.add(Marker(flag, path, ident))
    return found


def _report_problem(root: Path, target: str) -> "tuple[Optional[str], Optional[str]]":
    """Returns (header_stripped_body, problem) -- exactly one is None.
    `problem` is a human-readable reason the report cannot be trusted
    (missing/unreadable/empty/malformed header/non-zero build_exit), or
    None when the report is genuinely valid (including validly empty --
    zero warnings after the header line is a real result, not a problem)."""
    path = report_path(root, target)
    if not path.is_file():
        return None, "missing"
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError as exc:
        return None, f"unreadable ({exc})"
    if not text.strip():
        return None, "empty"

    first_line, _, rest = text.partition("\n")
    header = _HEADER_RE.match(first_line.strip())
    if not header:
        return None, f"malformed header ({first_line.strip()!r})"
    if header.group("exit") != "0":
        return None, (
            f"build_exit={header.group('exit')} (the -Wall capture build"
            " itself failed -- its diagnostic output cannot be trusted"
            " as complete)"
        )
    return rest, None


def is_report_valid(root: Path, target: str) -> bool:
    """True iff `target`'s report exists and passes the fail-closed
    contract -- i.e. a scan against it is real evidence, not a SKIP. Used
    by `warning_fence_{host,firmware}.scan_is_trustworthy` so
    `commands/fence_cmd.py`'s `--update-baseline` can refuse to prune a
    baseline off a scan that never actually ran (B1-1420 review finding:
    SKIP's empty result must never be mistaken for genuine shrinkage)."""
    _, problem = _report_problem(root, target)
    return problem is None


def scan_report(root: Path, target: str) -> Set[Marker]:
    """The one entry point both family modules call. Enforces the
    fail-closed report contract described in the module docstring, then
    hands the header-stripped body to `parse_gcc_warnings`."""
    rest, problem = _report_problem(root, target)

    if problem is not None:
        path = report_path(root, target)
        message = (
            f"warning_fence_{target}: report {problem} at {path} -- "
            "scripts/warn_scan.sh must produce a valid, build-verified"
            " report before this scan can run (B1-1420: absence of evidence"
            " is not evidence of zero warnings)"
        )
        if is_strict():
            raise WarningReportInvalidError(message)
        print(
            f"SKIP [fence:warning_fence_{target}]: {message} -- not"
            " enforced here (BB_WARNING_FENCE_STRICT unset); the enforcing"
            f" authority is `.github/workflows/build.yml`'s job that runs"
            f" `scripts/warn_scan.sh {target}` immediately before this"
            " check. Run that yourself first to check locally.",
            file=sys.stderr,
        )
        return set()

    return parse_gcc_warnings(rest)
