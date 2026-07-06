"""Shared CMakeLists.txt parsing — REQUIRES/PRIV_REQUIRES extraction and
`# bbtool-scaffold-hint:` comment parsing.

Lifted out of `commands/docs.py` (which imports `parse_requires` from here,
behavior unchanged) so `boards.py`'s build-graph derivation can reuse the same
parser instead of a second copy.

Determinism: sorted, de-duplicated lists; no dict/set iteration-order leaks.
"""
from __future__ import annotations
import re
from typing import Dict, List, Optional, Set, Tuple

_CMAKE_ARG_KEYWORDS = frozenset({
    "SRCS", "SRC_DIRS", "EXCLUDE_SRCS", "INCLUDE_DIRS", "PRIV_INCLUDE_DIRS",
    "REQUIRES", "PRIV_REQUIRES", "LDFRAGMENTS", "EMBED_FILES", "EMBED_TXTFILES",
    "KCONFIG", "KCONFIG_PROJBUILD", "WHOLE_ARCHIVE",
})

_SET_RE = re.compile(r'\bset\s*\(\s*(\w+)\s+(.*?)\)', re.DOTALL)
_LIST_APPEND_RE = re.compile(r'\blist\s*\(\s*APPEND\s+(\w+)\s+(.*?)\)', re.DOTALL)
_VAR_TOKEN_RE = re.compile(r'^\$\{(\w+)\}$')

_HINT_RE = re.compile(r'#\s*bbtool-scaffold-hint:\s*(\w+)\s*=\s*(.+?)\s*$')

_IF_OPEN_RE = re.compile(r'\bif\s*\(')
_IF_CLOSE_RE = re.compile(r'\bendif\s*\(')


class ConditionalSetError(Exception):
    """Raised when a `set(VAR ...)` call is found inside an `if()/elseif()/
    else()` block. Evaluating CMake conditionals is out of scope for this
    parser (no component needs it today); rather than silently picking
    whichever branch is textually last, this fails loud so a future
    conditional-set doesn't get mis-derived."""


def _conditional_set_spans(cmake_text: str) -> List[Tuple[int, int]]:
    """Return [(start, end), ...] character spans covered by `if(...)` ...
    `endif(...)` blocks, via a simple depth counter (nesting-aware, no
    elseif/else distinction needed since the whole if/endif span is opaque)."""
    spans: List[Tuple[int, int]] = []
    depth = 0
    start = None
    # Walk opens and closes in source order together.
    events = [(m.start(), 1) for m in _IF_OPEN_RE.finditer(cmake_text)]
    events += [(m.start(), -1) for m in _IF_CLOSE_RE.finditer(cmake_text)]
    events.sort(key=lambda e: e[0])
    for pos, delta in events:
        if delta == 1:
            if depth == 0:
                start = pos
            depth += 1
        else:
            depth -= 1
            if depth == 0 and start is not None:
                spans.append((start, pos))
                start = None
    return spans


def strip_cmake_line_comment(line: str) -> str:
    """Strip a CMake `#`-to-end-of-line comment from a single physical line.
    Defensive against `#` inside a quoted string (not observed in practice,
    but tracked so a stray `#` in a quoted arg isn't mistaken for a comment)."""
    out = []
    in_quotes = False
    for c in line:
        if c == '"':
            in_quotes = not in_quotes
            out.append(c)
        elif c == '#' and not in_quotes:
            break
        else:
            out.append(c)
    return "".join(out)


def strip_cmake_comments(cmake_text: str) -> str:
    """Strip `#`-to-end-of-line comments from every physical line."""
    return "\n".join(strip_cmake_line_comment(line) for line in cmake_text.splitlines())


def _resolve_vars(cmake_text: str) -> Tuple[Dict[str, List[str]], Set[str]]:
    """Resolve simple `set(VAR tok...)` / `list(APPEND VAR tok...)` calls into
    a var-name -> token-list map, in source order (later calls layer onto
    earlier ones, mirroring CMake's sequential execution). Also returns the
    set of var names that had at least one `set(VAR ...)` call inside an
    `if()/elseif()/else()/endif()` block ("conditionally-set" vars).

    `list(APPEND ...)` inside an `if()/endif()` block is applied unconditionally
    regardless (it only ever ADDS to a var, so applying it regardless of the
    branch is a safe over-approximation for scaffold's "make it link" purpose
    — any spuriously-included token that isn't a real project component gets
    filtered out downstream by boards.py's universe intersection).

    A `set(VAR ...)` inside an `if()/elseif()/else()/endif()` block is
    DIFFERENT: it can REPLACE the var's value depending on which branch
    executes, so "whichever branch is textually last wins" (this function's
    behavior for such vars) would be silently wrong if that value then feeds
    REQUIRES/PRIV_REQUIRES. Proper conditional evaluation is out of scope (no
    component needs it today); the caller (`parse_requires`) raises
    `ConditionalSetError` if a conditionally-set var from the returned set is
    actually expanded inside REQUIRES/PRIV_REQUIRES — a var that's
    conditionally set but only used for e.g. SRCS is unaffected."""
    if_spans = _conditional_set_spans(cmake_text)

    def _in_conditional(pos: int) -> bool:
        return any(start <= pos < end for start, end in if_spans)

    calls = []
    conditional_vars: Set[str] = set()
    for m in _SET_RE.finditer(cmake_text):
        if _in_conditional(m.start()):
            conditional_vars.add(m.group(1))
        calls.append((m.start(), "set", m.group(1), m.group(2)))
    for m in _LIST_APPEND_RE.finditer(cmake_text):
        calls.append((m.start(), "append", m.group(1), m.group(2)))
    calls.sort(key=lambda c: c[0])

    resolved: Dict[str, List[str]] = {}
    for _, kind, name, body in calls:
        tokens = [t.strip('"') for t in body.split()]
        if kind == "set":
            resolved[name] = tokens
        else:
            resolved.setdefault(name, []).extend(tokens)
    return resolved, conditional_vars


def _expand_token(tok: str, resolved_vars: Dict[str, List[str]]) -> List[str]:
    """Expand a single `${VAR}` token via resolved_vars; a literal token (or
    an unresolvable `${VAR}`) passes through unchanged."""
    m = _VAR_TOKEN_RE.match(tok)
    if not m:
        return [tok]
    return resolved_vars.get(m.group(1), [tok])


def parse_requires(cmake_text: str, component: Optional[str] = None) -> Tuple[List[str], List[str]]:
    """Parse REQUIRES / PRIV_REQUIRES args out of an idf_component_register(...)
    call. Returns (requires, priv_requires) — sorted, de-duplicated lists.
    `${VAR}` tokens (from a preceding `set()`/`list(APPEND)` in the same
    file) are expanded before classification. `component` (optional) names
    the owning component in the error message if a REQUIRES/PRIV_REQUIRES
    `${VAR}` resolves to a conditionally-set variable — see `_resolve_vars`.
    Raises `ConditionalSetError` in that case (a var conditionally `set()`
    but used only for e.g. SRCS is unaffected)."""
    stripped = strip_cmake_comments(cmake_text)
    resolved_vars, conditional_vars = _resolve_vars(stripped)

    m = re.search(r'idf_component_register\s*\(', stripped)
    if not m:
        return [], []
    start = stripped.index('(', m.start())
    depth = 0
    end = -1
    for i in range(start, len(stripped)):
        c = stripped[i]
        if c == '(':
            depth += 1
        elif c == ')':
            depth -= 1
            if depth == 0:
                end = i
                break
    if end == -1:
        return [], []

    block = stripped[start + 1:end]
    tokens = block.split()

    requires: set = set()
    priv_requires: set = set()
    current = None
    for tok in tokens:
        if tok in _CMAKE_ARG_KEYWORDS:
            current = tok
            continue
        if current in ("REQUIRES", "PRIV_REQUIRES"):
            var_m = _VAR_TOKEN_RE.match(tok)
            if var_m and var_m.group(1) in conditional_vars:
                where = f"component '{component}'" if component else "this CMakeLists.txt"
                raise ConditionalSetError(
                    f"{where}: {current} references ${{{var_m.group(1)}}}, which is "
                    f"set(...) inside an if()/elseif()/else() block — conditional "
                    "set() feeding REQUIRES/PRIV_REQUIRES is not supported by the "
                    "scaffold parser (it would silently pick whichever branch is "
                    "textually last). Use list(APPEND ...) inside the conditional "
                    "instead, seeded by an unconditional set() before the if()."
                )
        for clean in (t.strip('"') for t in _expand_token(tok, resolved_vars)):
            if current == "REQUIRES":
                requires.add(clean)
            elif current == "PRIV_REQUIRES":
                priv_requires.add(clean)

    return sorted(requires), sorted(priv_requires)


def parse_hints(cmake_text: str) -> Dict[str, List[str]]:
    """Parse `# bbtool-scaffold-hint: key=val` comment lines out of a
    component's own CMakeLists.txt (searched over the ORIGINAL text, before
    comment-stripping). Returns key -> [val, ...] (insertion order, repeated
    keys accumulate); e.g. `include=platform/host/bb_foo/extra` and
    `source=components/bb_foo/legacy/bb_foo_shim.c`."""
    hints: Dict[str, List[str]] = {}
    for line in cmake_text.splitlines():
        m = _HINT_RE.search(line)
        if m:
            hints.setdefault(m.group(1), []).append(m.group(2))
    return hints
