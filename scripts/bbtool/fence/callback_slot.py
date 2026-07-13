"""callback_slot fence family — ratchet-fence for hand-rolled reimplementations
of the `bb_core` "single-slot injected callback" idiom.

`bb_core` (components/bb_core/include/bb_callback_slot.h) extracted a small,
previously-duplicated idiom into three macros (`BB_CALLBACK_SLOT_RET`,
`BB_CALLBACK_SLOT_VOID0`, `BB_CALLBACK_SLOT_VOID`): a file-static callback
slot + a public setter that composition wires + a public, null-safe
"invoke". This fence freezes the CURRENT set of hand-rolled reimplementations
as a draining baseline: no *new* one may appear outside the canonical macro,
but existing sites are grandfathered until migrated.

One family per shared helper (see also `fence/clamp.py`, `fence/sat_sub.py`)
— the natural "family = module" use of the generic fence engine
(`fence/_base.py`).

Marker type scanned: `callback_slot` — a hand-rolled instance of the idiom:
(1) a file-static callback-pointer declaration `static <cb_type> <name> =
NULL;` (either raw function-pointer syntax, or a typedef'd function-pointer
type resolved via a first-pass typedef table built from every `**/*.h`
under components/+platform) whose resolved signature is one of the two
shapes the macro set actually supports — void return (with or without
args, i.e. BB_CALLBACK_SLOT_VOID/VOID0) or a non-void return with NO args
(BB_CALLBACK_SLOT_RET); (2) a same-file plain-assignment setter statement
`<name> = <param>;` that is the FIRST statement of a function whose own
parameter list is exactly one parameter.

Does NOT match `BB_CALLBACK_SLOT_*(...)` macro instantiations: those are a
single macro-call line at the use site (e.g. `BB_CALLBACK_SLOT_VOID0(...)`)
with no literal `static ... = NULL;` declaration or hand-written setter
definition in the raw source text for this scanner to see — the shape only
exists after preprocessing.

ACCEPTED LIMITATIONS:

1. Return-value + argument-carrying callbacks (e.g. bb_prov's save
   callback, bb_ws_server's frame handler, the several malloc/calloc
   "allocator override" hooks scattered across bb_http_body/bb_response/
   bb_mqtt_client/bb_timer/bb_core's own bb_mem host allocator) are out of
   scope — none of the three BB_CALLBACK_SLOT_* macro variants support that
   shape (RET is no-arg only; VOID/VOID0 never return a value), so flagging
   them as "must migrate" would be misleading given there is nowhere for
   them to migrate TO today. A future macro variant could close this gap;
   until then this fence doesn't track them.
2. ctx-carrying two-parameter setters (bb_fan's autofan persist hook,
   bb_ws_server's connect/disconnect hooks, bb_ota_check's combined
   pause+resume setter) are a different, richer idiom (opaque userdata
   alongside the callback) that the current macro set also doesn't cover
   (every generated setter takes exactly one parameter) — excluded by the
   same single-parameter-setter arity check, not tracked either.
3. Typedef resolution is a single textual pass over `**/*.h` matching
   `typedef <ret> (*<name>)(<args>);` — an alias-of-alias typedef (e.g.
   `typedef bb_http_pause_cb_t bb_ota_check_pause_cb_t;`) or any other
   exotic spelling silently fails to resolve, and the declaration is
   skipped (safe under-match direction, same posture as every other family
   here).
4. `platform/espidf/bb_wifi/bb_wifi.c`'s `on_got_ip` slot IS included
   despite its setter carrying extra late-registration replay logic beyond
   the plain assignment — the assignment search only requires the
   plain-assign statement to be the setter's FIRST statement, not its ONLY
   one, so the replay logic doesn't suppress detection. It's deliberately
   grandfathered (not migrated in #803): a future macro variant with replay
   support would be needed before it can move off this fence.
5. Test-only hooks are out of scope, mirroring `fence/_base.py`'s
   `EXCLUDE_DIRS` treatment of `test/`/`tests/` directories: a declaration
   or setter that sits inside an enclosing `#if`/`#ifdef`/`#ifndef` block
   whose condition text contains `_TESTING`, `_TEST`, or `UNIT_TEST`
   (case-insensitive) is skipped (see `_build_testing_line_flags` — a
   simple per-line `#if*`/`#elif`/`#else`/`#endif` stack, not a real
   preprocessor). This is a semantic signal (the actual compile-time
   gate), not a naming convention, so it also covers a testing hook that
   happens not to have `_test_` in its own symbol name. Two known
   limitations, both conservative (they can only cause OVER-exclusion of
   testing code or OVER-counting of an edge case as production — never a
   dropped production site):
   - An inverted guard (`#ifndef X_TESTING` with the testing code in the
     `#else` branch) is not recognized — the scanner only treats the TRUE
     branch of a testing-matching condition as the testing region, which
     is the shape every current guard in this codebase uses (`#ifdef
     BB_CACHE_TESTING`).
   - An `#elif` branch is ALWAYS treated as production, regardless of its
     own condition text (no re-evaluation of `_TESTING_GUARD_RE` per
     `#elif` branch — there's no real preprocessor here to track which of
     several `#elif` conditions is "true"). This is the safe direction: a
     production slot in an `#elif` branch of a testing `#ifdef` is
     correctly scanned normally (never silently dropped from the fence —
     the bug this limitation replaces), at the cost of a testing hook
     that itself lives in an `#elif defined(FOO_TESTING)` branch being
     over-counted (seeded/frozen as if production) rather than excluded.
6. Identity is `component:file-stem:name`, NOT `component:enclosing-
   symbol:var` like `clamp`/`sat_sub` — deliberately, because two different
   platform-variant files in the same component (bb_wifi_r4.cpp,
   bb_wifi_cc3000.cpp) hand-roll a slot of the identical setter/name and
   would collide under the enclosing-symbol scheme (see `_stem_of` below).
   The tradeoff: this family does NOT get the generic fence's "a pure file
   rename never trips the fence" guarantee — renaming one of these source
   files changes its identity, which surfaces as an ordinary remove+add on
   the next `--update-baseline` (not a hard failure, but real baseline
   churn for what is otherwise a no-op change).
"""
from __future__ import annotations
import re
import sys
from pathlib import Path
from typing import Dict, List, Optional, Set, Tuple

from fence import _base
from fence._base import Marker

_SCAN_ROOTS = ("components", "platform")
_SRC_GLOBS = ["**/*.c", "**/*.h", "**/*.cpp"]
_HDR_GLOBS = ["**/*.h"]

# The canonical helper's own definition file — its macro bodies contain
# `static cb_type bb_callback_slot_##slot = NULL;` as macro-expansion text,
# not a real declaration; the `##` token-paste operator already keeps our
# regexes from matching it (see module docstring point 2), but excluding
# the file explicitly documents the intent and matches this codebase's
# established per-family canonical-impl exclusion convention (see
# `clamp.py`'s `_CANONICAL_CLAMP_PREFIXES`, `scalar_parse.py`'s
# `_SCALAR_PARSE_EXCLUDE_PREFIXES`).
_CANONICAL_PREFIXES = ("components/bb_core/",)

# ---------------------------------------------------------------------------
# Marker-type -> reporting bucket
# ---------------------------------------------------------------------------

_BUCKETS = {
    "callback_slot": "hand-rolled callback_slot",
}


def _bucket_for(marker_type: str) -> str:
    return _BUCKETS.get(marker_type, marker_type)


def counts_by_bucket(markers: Set[Marker]) -> dict:
    return _base.counts_by_bucket(markers, bucket_fn=_bucket_for)


# ---------------------------------------------------------------------------
# "owning component" + file-stem — see the module docstring: two different
# platform-variant files in the SAME component (e.g. bb_wifi_r4.cpp and
# bb_wifi_cc3000.cpp both under platform/arduino/bb_wifi/) can legitimately
# hand-roll a slot of the identical name; keying purely on component+name
# would collapse those into one ratchet entry and silently stop tracking
# one of them. Including the file stem keeps each platform variant's site
# distinct while still surviving an unrelated line-shift/reformat within
# the same file (identity never includes a line number).
# ---------------------------------------------------------------------------

def _component_of(rel_path: str) -> str:
    parts = Path(rel_path).parts
    if len(parts) >= 2 and parts[0] == "components":
        return parts[1]
    if len(parts) >= 3 and parts[0] == "platform":
        return parts[2]
    return parts[0] if parts else rel_path


def _stem_of(rel_path: str) -> str:
    return Path(rel_path).stem


# ---------------------------------------------------------------------------
# Test-only-hook exclusion — a per-line `#if`/`#ifdef`/`#ifndef` /
# `#elif`/`#else`/`#endif` stack (not a real preprocessor) that marks every
# line inside the TRUE branch of a condition whose text contains
# `_TESTING`, `_TEST`, or `UNIT_TEST` (case-insensitive) as a "testing"
# line. See ACCEPTED LIMITATIONS #5 above.
# ---------------------------------------------------------------------------

_TESTING_GUARD_RE = re.compile(r'_TESTING|_TEST\b|UNIT_TEST', re.IGNORECASE)
_IF_DIRECTIVE_RE = re.compile(r'^\s*#\s*(?:ifdef|ifndef|if)\b(.*)$')
_ELIF_DIRECTIVE_RE = re.compile(r'^\s*#\s*elif\b')
_ELSE_DIRECTIVE_RE = re.compile(r'^\s*#\s*else\b')
_ENDIF_DIRECTIVE_RE = re.compile(r'^\s*#\s*endif\b')


def _build_testing_line_flags(lines: List[str]) -> List[bool]:
    flags = [False] * len(lines)
    stack: List[dict] = []
    for i, line in enumerate(lines):
        m = _IF_DIRECTIVE_RE.match(line)
        if m:
            own_matches = bool(_TESTING_GUARD_RE.search(m.group(1)))
            stack.append({"own_matches": own_matches, "in_else": False})
        elif _ELIF_DIRECTIVE_RE.match(line):
            # An `#elif` branch is never treated as the frame's testing
            # branch, regardless of its own condition text: we don't
            # attempt to re-evaluate `own_matches` per-branch (no real
            # preprocessor here), so an `#elif` is conservatively always
            # scanned as PRODUCTION -- the safe direction is to
            # over-exclude a testing region, never to wrongly exclude
            # production. Reusing `in_else` (rather than a separate
            # tri-state) is deliberate: an `#elif` behaves exactly like an
            # `#else` for our purposes (frame no longer the testing
            # branch), and this also correctly handles a subsequent
            # `#else` after the `#elif` (already `in_else=True`, a no-op).
            if stack:
                stack[-1]["in_else"] = True
        elif _ELSE_DIRECTIVE_RE.match(line):
            if stack:
                stack[-1]["in_else"] = True
        elif _ENDIF_DIRECTIVE_RE.match(line):
            if stack:
                stack.pop()
        flags[i] = any(f["own_matches"] and not f["in_else"] for f in stack)
    return flags


# ---------------------------------------------------------------------------
# Pass 1: build a typedef_name -> (ret_type, args_str) table by scanning
# every header for `typedef <ret> (*<name>)(<args>);`. Best-effort, single
# indirection only (see ACCEPTED LIMITATIONS #3 above).
# ---------------------------------------------------------------------------

_TYPEDEF_FUNCPTR_RE = re.compile(
    r'typedef\s+([\w][\w \*]*?)\s*\(\s*\*\s*(\w+)\s*\)\s*\(([^;]*?)\)\s*;'
)


def _build_typedef_table(root: Path) -> Dict[str, Tuple[str, str]]:
    table: Dict[str, Tuple[str, str]] = {}
    for path in _base.iter_files(root, _SCAN_ROOTS, _HDR_GLOBS):
        text = _base.read(path)
        for m in _TYPEDEF_FUNCPTR_RE.finditer(text):
            ret_type, name, args = m.group(1).strip(), m.group(2), m.group(3).strip()
            table.setdefault(name, (ret_type, args))
    return table


# ---------------------------------------------------------------------------
# Declaration scanning: raw function-pointer syntax vs. typedef-bare form.
# ---------------------------------------------------------------------------

_RAW_DECL_RE = re.compile(
    r'(?m)^[ \t]*static\s+([\w][\w \*]*?)\s*\(\s*\*\s*(\w+)\s*\)\s*\(([^;]*?)\)\s*=\s*NULL\s*;'
)

_TYPEDEF_DECL_RE = re.compile(
    r'(?m)^[ \t]*static\s+([A-Za-z_]\w*)\s+(\w+)\s*=\s*NULL\s*;'
)


def _shape_supported(ret_type: str, args: str) -> bool:
    """True iff (ret_type, args) matches one of the two shapes the
    BB_CALLBACK_SLOT_* macro set actually supports: void-return (any args,
    covers VOID/VOID0) or non-void-return with NO args (covers RET).
    A non-void return WITH args is the one shape none of the three macros
    can express — see ACCEPTED LIMITATIONS #1."""
    ret_type = ret_type.strip()
    no_args = args.strip() in ("", "void")
    if ret_type == "void":
        return True
    return no_args


# ---------------------------------------------------------------------------
# Setter validation: the plain-assignment statement must be the FIRST
# statement of a function whose own parameter list is exactly one
# parameter (top-level comma count, correctly ignoring commas nested inside
# a function-pointer parameter's own argument list) — see ACCEPTED
# LIMITATIONS #2.
# ---------------------------------------------------------------------------

_ASSIGN_TERMINATOR_RE = re.compile(r'\s*=\s*(\w+)\s*;')


def _match_open_paren_backward(text: str, close_idx: int) -> Optional[int]:
    depth = 0
    i = close_idx
    while i >= 0:
        c = text[i]
        if c == ')':
            depth += 1
        elif c == '(':
            depth -= 1
            if depth == 0:
                return i
        i -= 1
    return None


def _count_top_level_params(params_text: str) -> int:
    params_text = params_text.strip()
    if not params_text or params_text == "void":
        return 0
    depth = 0
    count = 1
    for c in params_text:
        if c == '(':
            depth += 1
        elif c == ')':
            depth -= 1
        elif c == ',' and depth == 0:
            count += 1
    return count


def _is_single_param_plain_setter(text: str, name: str, assign_start: int) -> bool:
    """Given the start index of an `<name> = <ident>;` match, confirms it is
    the FIRST statement inside a function body opened by the nearest
    preceding `{`, and that function's own parameter list is exactly one
    parameter."""
    brace_idx = text.rfind('{', 0, assign_start)
    if brace_idx == -1:
        return False
    if text[brace_idx + 1:assign_start].strip() != "":
        return False  # not the first statement in this body
    j = brace_idx - 1
    while j >= 0 and text[j].isspace():
        j -= 1
    if j < 0 or text[j] != ')':
        return False  # no signature closing paren immediately before `{`
    open_paren_idx = _match_open_paren_backward(text, j)
    if open_paren_idx is None:
        return False
    params_text = text[open_paren_idx + 1:j]
    return _count_top_level_params(params_text) == 1


def _find_setter_assignment(text: str, lines: List[str], testing_flags: List[bool], name: str) -> bool:
    # Deliberately NOT anchored to line-start (`^`) -- a one-line setter
    # definition (`void set_x(t cb) { s_x = cb; }`) has the assignment
    # preceded by the rest of the signature on the same physical line, not
    # by pure leading whitespace. `_is_single_param_plain_setter` is the
    # real gate (first-statement-of-a-one-param-function-body), not this
    # regex, which only needs to locate candidate `name = ident;` sites.
    pattern = re.compile(r'\b' + re.escape(name) + r'\s*=\s*(\w+)\s*;')
    for m in pattern.finditer(text):
        rhs = m.group(1)
        if rhs == "NULL":
            continue  # a reset, not a caller-supplied setter assignment
        line_idx = text.count("\n", 0, m.start())
        if _base.is_noise_line(lines[line_idx].strip()):
            continue  # a commented-out setter, e.g. `// void set_x(...) { s_x = h; }`
        if testing_flags[line_idx]:
            continue  # setter itself sits inside a #if*_TESTING/_TEST/UNIT_TEST block
        if _is_single_param_plain_setter(text, name, m.start()):
            return True
    return False


# ---------------------------------------------------------------------------
# callback_slot — hand-rolled single-slot injected callbacks.
# ---------------------------------------------------------------------------

def _scan_callback_slot(root: Path) -> Set[Marker]:
    typedef_table = _build_typedef_table(root)
    found: Set[Marker] = set()
    for path in _base.iter_files(root, _SCAN_ROOTS, _SRC_GLOBS):
        rel = _base.rel(root, path)
        if rel.startswith(_CANONICAL_PREFIXES):
            continue
        text = _base.read(path)
        lines = text.splitlines()
        testing_flags = _build_testing_line_flags(lines)

        for m in _RAW_DECL_RE.finditer(text):
            line_idx = text.count("\n", 0, m.start())
            if _base.is_noise_line(lines[line_idx].strip()):
                continue
            if testing_flags[line_idx]:
                continue  # test-only hook — see ACCEPTED LIMITATIONS #5
            ret_type, name, args = m.group(1), m.group(2), m.group(3)
            if not _shape_supported(ret_type, args):
                continue
            if not _find_setter_assignment(text, lines, testing_flags, name):
                continue
            component = _component_of(rel)
            stem = _stem_of(rel)
            found.add(Marker("callback_slot", rel, f"{component}:{stem}:{name}"))

        for m in _TYPEDEF_DECL_RE.finditer(text):
            line_idx = text.count("\n", 0, m.start())
            if _base.is_noise_line(lines[line_idx].strip()):
                continue
            if testing_flags[line_idx]:
                continue  # test-only hook — see ACCEPTED LIMITATIONS #5
            typedef_name, name = m.group(1), m.group(2)
            resolved = typedef_table.get(typedef_name)
            if resolved is None:
                continue  # unresolved typedef — safe under-match, see limitation #3
            ret_type, args = resolved
            if not _shape_supported(ret_type, args):
                continue
            if not _find_setter_assignment(text, lines, testing_flags, name):
                continue
            component = _component_of(rel)
            stem = _stem_of(rel)
            found.add(Marker("callback_slot", rel, f"{component}:{stem}:{name}"))
    return found


def scan_all(root: str) -> Set[Marker]:
    return _base.scan_all(sys.modules[__name__], root)
