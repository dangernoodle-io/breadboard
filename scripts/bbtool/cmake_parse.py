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

# Path-valued idf_component_register(...) keywords, in a FIXED tuple order
# (not just a frozenset) so parse_paths()'s returned dict always iterates in
# this same order -- a bare frozenset's iteration order depends on Python's
# per-process string-hash salt (PYTHONHASHSEED), which would leak
# non-determinism into any caller that iterates paths.items() (e.g. the
# component-path-unresolved lint rule's violation ordering). Used by
# parse_paths() (the component-path-unresolved lint rule); membership checks
# use the frozenset form below.
_PATH_ARG_KEYWORD_ORDER: Tuple[str, ...] = (
    "SRCS", "SRC_DIRS", "INCLUDE_DIRS", "PRIV_INCLUDE_DIRS",
)
_PATH_ARG_KEYWORDS = frozenset(_PATH_ARG_KEYWORD_ORDER)

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


def _resolve_vars_branches(cmake_text: str) -> Dict[str, List[List[str]]]:
    """Like `_resolve_vars`'s single "last set() wins, then APPENDed"
    resolution, but returns EVERY `set(VAR ...)` call's own token list as a
    DISTINCT branch instead of collapsing to one -- an if()/elseif()/else()
    ladder's N branches become N separate lists for VAR, in source order,
    rather than "whichever is textually last". Used by every path-bearing-
    command parser's conditional handling (`_expand_path_token`, in turn
    used by `parse_paths`, `parse_target_include_directories`,
    `parse_include_calls`): every branch's paths must resolve on disk
    regardless of which is build-time-live, so there is nothing to guess,
    unlike REQUIRES/PRIV_REQUIRES -- see `parse_paths`'s docstring.
    `parse_requires` stays on `_resolve_vars` and its `ConditionalSetError`
    posture, untouched by this function.

    Every `list(APPEND VAR ...)` call (conditional or not) is applied to
    EVERY branch collected for VAR, REGARDLESS OF SOURCE ORDER between the
    `set()` calls and the `list(APPEND ...)` calls -- two passes: first
    collect every `set(VAR ...)` call as its own branch, then accumulate
    every `list(APPEND VAR ...)` call's tokens (in their own source order)
    into one combined per-var append suffix, then extend every branch with
    that whole suffix. An `list(APPEND VAR ...)` that textually PRECEDES
    the var's conditional `set()` branches must still extend those
    branches, not just seed an orphan branch of its own (that was a real
    bug in an earlier single-pass version of this function: an append seen
    before any set() call created a phantom branch containing ONLY the
    appended tokens, and a set() branch added afterward never got extended
    -- silently under-covering the append-then-branch overlap case, though
    the phantom branch itself made the bug over-inclusive rather than a
    coverage hole). If a var has append calls but zero `set()` branches, it
    still seeds exactly one branch from the append tokens (unchanged
    behavior for that case) -- the same unconditional over-approximation
    `_resolve_vars` already documents for list(APPEND) (it only ever adds,
    so applying it to every branch is a safe over-approximation, never a
    silent narrowing)."""
    branches: Dict[str, List[List[str]]] = {}
    for m in _SET_RE.finditer(cmake_text):
        name = m.group(1)
        tokens = [t.strip('"') for t in m.group(2).split()]
        branches.setdefault(name, []).append(tokens)

    appends: Dict[str, List[str]] = {}
    for m in _LIST_APPEND_RE.finditer(cmake_text):
        name = m.group(1)
        tokens = [t.strip('"') for t in m.group(2).split()]
        appends.setdefault(name, []).extend(tokens)

    for name, appended_tokens in appends.items():
        if name not in branches:
            branches[name] = [[]]
        for branch in branches[name]:
            branch.extend(appended_tokens)

    return branches


def _expand_path_token(tok: str, resolved_vars: Dict[str, List[str]],
                        conditional_vars: Set[str],
                        branch_vars: Dict[str, List[List[str]]]) -> List[str]:
    """Expand one path-bearing token: an exact `${VAR}` reference to a
    conditionally `set()` var enumerates EVERY branch (via
    `_resolve_vars_branches`) rather than guessing which is build-time-live
    -- shared by every path-bearing-command parser (`parse_paths`,
    `parse_target_include_directories`, `parse_include_calls`) so a
    conditional path in ANY of them gets the same "check every branch"
    treatment, never a silent last-branch-wins guess. Anything else expands
    the same way `_expand_token` always has."""
    var_m = _VAR_TOKEN_RE.match(tok)
    if var_m and var_m.group(1) in conditional_vars:
        out: List[str] = []
        for branch_tokens in branch_vars.get(var_m.group(1), []):
            out.extend(t.strip('"') for t in branch_tokens)
        return out
    return [t.strip('"') for t in _expand_token(tok, resolved_vars)]


def _expand_token(tok: str, resolved_vars: Dict[str, List[str]]) -> List[str]:
    """Expand a single `${VAR}` token via resolved_vars; a literal token (or
    an unresolvable `${VAR}`) passes through unchanged."""
    m = _VAR_TOKEN_RE.match(tok)
    if not m:
        return [tok]
    return resolved_vars.get(m.group(1), [tok])


def _iter_call_blocks(stripped_text: str, fn_names):
    """Yield `(fn_name, block)` pairs for every call to any command in
    `fn_names` (a single name `str`, or an iterable of names) found in
    `stripped_text`, MERGED into one combined source-order stream (not
    grouped per name) -- `block` is the paren-depth-balanced argument text
    inside that call's outer parens.

    Generalizes the single-name lookup `_register_call_block` (which wants
    only the FIRST `idf_component_register(...)` call) to support (a) a
    function called more than once per file (e.g. `bb_embed_assets(...)`)
    and (b) checking several different path-bearing command names in one
    pass -- so a lint rule that needs to cover another CMake command later
    adds its name to a set, not a new code path (see the
    `component-path-unresolved` rule's docstring for the full command
    inventory and the deliberately-excluded commands it documents).

    KNOWN LIMITATION (pre-existing, not fixed here — reproduces against
    499cbc0e too, before any of B1-1134's command-coverage additions):
    this scan is comment-stripped (`strip_cmake_comments`) but NOT
    string-literal-aware -- a command name that happens to appear inside a
    quoted CMake string (e.g. `message("see include(...) docs")`) is
    misdetected as a real call and produces a bogus/malformed "block". No
    such string exists in this codebase's `components/**/CMakeLists.txt`
    today, but adding `target_include_directories`/`include` to the
    scanned command set (B1-1134) widens the surface a future string
    literal could hit.

    The CONSEQUENCE of a misdetected call is NOT uniform across every
    consumer of this function -- do not assume it's always safe:
      - For every PATH-TOKEN consumer of `_iter_call_blocks` (`parse_paths`
        via `_register_call_block`, `parse_target_include_directories`,
        `parse_include_calls`, `parse_embed_assets`), a bogus block either
        fails paren-balancing (yields nothing further for that match) or
        produces nonsense "tokens" that won't resolve to a real on-disk
        path -- the caller reports that as a LOUD violation, never a
        swallowed one.
      - `parse_var_assignment_events` (and its `_PROPERTY_GET_RE` regex)
        do NOT go through `_iter_call_blocks` at all -- they scan
        `stripped_text` directly with the same comment-stripped-but-not-
        string-literal-aware limitation, but for a DIFFERENT and WORSE
        consequence: a property-get "call" spelled inside a quoted string
        (e.g. `message("workaround: idf_component_get_property(bogus_var
        some_component COMPONENT_DIR)")`) is indistinguishable from a real
        one, and grants `single_opaque_property_vars` a real, SILENT
        exemption for a path that can never resolve -- see that function's
        own docstring for the full caveat and reproduction.

    A genuine string-literal-aware tokenizer is intentionally NOT
    attempted here -- it's a larger, separately-scoped change (SRCS/
    INCLUDE_DIRS tokens are THEMSELVES quoted strings, so a wholesale
    "strip string literals" pass would break path-token extraction, not
    just the property-get detection; a correct fix needs to distinguish
    "inside a string that's itself an argument we want" from "inside a
    string that's just prose"). Tracked as B1-1139 rather than folded
    into this commit."""
    if isinstance(fn_names, str):
        fn_names = (fn_names,)
    found = []
    for fn_name in fn_names:
        pattern = re.compile(r'\b' + re.escape(fn_name) + r'\s*\(')
        pos = 0
        while True:
            m = pattern.search(stripped_text, pos)
            if not m:
                break
            start = stripped_text.index('(', m.start())
            depth = 0
            end = -1
            for i in range(start, len(stripped_text)):
                c = stripped_text[i]
                if c == '(':
                    depth += 1
                elif c == ')':
                    depth -= 1
                    if depth == 0:
                        end = i
                        break
            if end == -1:
                break
            found.append((start, fn_name, stripped_text[start + 1:end]))
            pos = end + 1
    found.sort(key=lambda t: t[0])
    for _, fn_name, block in found:
        yield fn_name, block


def _register_call_block(stripped_text: str) -> Optional[str]:
    """Return the token text inside the FIRST `idf_component_register(...)`
    call's outer, paren-depth-balanced parens, or `None` if the file has no
    such call (there is only ever one per file in practice). Shared by
    `parse_requires` and `parse_paths` so the call-boundary detection lives
    in exactly one place."""
    for _fn_name, block in _iter_call_blocks(stripped_text, "idf_component_register"):
        return block
    return None


def _iter_arg_tokens(block: str, keywords: frozenset = _CMAKE_ARG_KEYWORDS):
    """Yield `(keyword, token)` pairs walking `block`'s whitespace-split
    tokens left to right, tracking the current recognized-`keywords` arg
    name as of each token (default `_CMAKE_ARG_KEYWORDS`, the
    `idf_component_register(...)` keyword set -- pass a different set, e.g.
    `bb_embed_assets(...)`'s `OUT_SRCS`/`ASSETS`, for a different call
    shape). Tokens before the first recognized keyword (the call's leading
    positional argument, if any) are skipped. Each yielded token has its
    surrounding CMake quotes stripped (e.g. `"${VAR}"` yields `${VAR}`)
    BEFORE the keyword membership check -- a bare keyword is never quoted in
    practice, so this is a no-op for keyword detection, but it's
    load-bearing for `${VAR}` tokens: SRCS/INCLUDE_DIRS path lists routinely
    quote their `${VAR}` entries (`"${BACKEND_SRC}"`) while REQUIRES lists
    don't, and an un-stripped quoted token fails BOTH
    `_check_conditional_var`'s and `_expand_token`'s `_VAR_TOKEN_RE` exact
    match, silently falling through to treat the literal `"${VAR}"` text as
    an unexpandable, unresolvable path -- exactly the class of defect
    component-path-unresolved exists to catch, so this parser must not
    introduce it itself. Shared token-walk used by `parse_requires`,
    `parse_paths`, and `parse_embed_assets`."""
    current = None
    for raw_tok in block.split():
        tok = raw_tok.strip('"')
        if tok in keywords:
            current = tok
            continue
        if current is not None:
            yield current, tok


def _check_conditional_var(tok: str, current: str, conditional_vars: Set[str],
                            component: Optional[str]) -> None:
    """Raise `ConditionalSetError` if `tok` is an exact `${VAR}` reference to
    a var in `conditional_vars` — used by `parse_requires` for REQUIRES/
    PRIV_REQUIRES, where picking a branch would be an outright guess (the
    branches mean different, mutually-exclusive things and only one is
    build-time-live). `parse_paths` does NOT use this — a path only needs
    to resolve on disk, and every branch's path must resolve regardless of
    which is live, so it branch-enumerates instead of guessing (see
    `_resolve_vars_branches` and `parse_paths`'s docstring)."""
    var_m = _VAR_TOKEN_RE.match(tok)
    if var_m and var_m.group(1) in conditional_vars:
        where = f"component '{component}'" if component else "this CMakeLists.txt"
        raise ConditionalSetError(
            f"{where}: {current} references ${{{var_m.group(1)}}}, which is "
            f"set(...) inside an if()/elseif()/else() block — conditional "
            "set() feeding this argument is not supported by the scaffold "
            "parser (it would silently pick whichever branch is textually "
            "last). Use list(APPEND ...) inside the conditional instead, "
            "seeded by an unconditional set() before the if()."
        )


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

    block = _register_call_block(stripped)
    if block is None:
        return [], []

    requires: set = set()
    priv_requires: set = set()
    for current, tok in _iter_arg_tokens(block):
        if current not in ("REQUIRES", "PRIV_REQUIRES"):
            continue
        _check_conditional_var(tok, current, conditional_vars, component)
        for clean in (t.strip('"') for t in _expand_token(tok, resolved_vars)):
            if current == "REQUIRES":
                requires.add(clean)
            else:
                priv_requires.add(clean)

    return sorted(requires), sorted(priv_requires)


def parse_paths(cmake_text: str, component: Optional[str] = None) -> Dict[str, List[str]]:
    """Parse SRCS / SRC_DIRS / INCLUDE_DIRS / PRIV_INCLUDE_DIRS path-token
    args out of an idf_component_register(...) call. Returns a dict keyed on
    every `_PATH_ARG_KEYWORDS` name (always all four keys present, even if
    empty) -> `[token, ...]` in source order, NOT deduped or sorted — unlike
    `parse_requires`'s component-name sets, path identity/order/duplication
    is a caller concern (e.g. the component-path-unresolved lint rule
    resolving each token against disk), not something this parser should
    collapse.

    `${VAR}` tokens are expanded the same way `parse_requires` expands
    REQUIRES/PRIV_REQUIRES: via a preceding `set()`/`list(APPEND)` in the
    same file. `${CMAKE_CURRENT_LIST_DIR}` is NEVER resolved here (it names
    the component's own on-disk directory, which this text-only parser has
    no way to know) — it passes through as a literal substring in the
    returned tokens for the caller to substitute.

    UNLIKE `parse_requires`, a path keyword's `${VAR}` referencing a
    variable `set()` inside an `if()/elseif()/else()` block does NOT raise
    `ConditionalSetError` here -- it BRANCH-ENUMERATES instead, via
    `_resolve_vars_branches`, appending every branch's own token(s) to the
    result. REQUIRES/PRIV_REQUIRES genuinely can't pick a branch (the
    branches mean different, mutually-exclusive things and only one is
    build-time-live -- guessing would be wrong), but a path only needs to
    resolve on disk, and EVERY branch's path must resolve regardless of
    which is live (there's a real file at both the ESP-IDF and host backend
    paths, e.g.) -- so there's nothing to guess, and enumerating every
    branch is strictly MORE coverage than picking one, never less. `component`
    is currently unused by this path (kept for signature symmetry with
    `parse_requires` and because a future genuinely-unenumerable branch
    shape may want it) -- see the module's `ConditionalSetError` docstring
    for why REQUIRES/PRIV_REQUIRES still needs to refuse to guess.

    Missing `idf_component_register(...)` call returns all-empty lists,
    never raises."""
    stripped = strip_cmake_comments(cmake_text)
    resolved_vars, conditional_vars = _resolve_vars(stripped)
    branch_vars = _resolve_vars_branches(stripped)

    result: Dict[str, List[str]] = {k: [] for k in _PATH_ARG_KEYWORD_ORDER}

    block = _register_call_block(stripped)
    if block is None:
        return result

    for current, tok in _iter_arg_tokens(block):
        if current not in _PATH_ARG_KEYWORDS:
            continue
        result[current].extend(
            _expand_path_token(tok, resolved_vars, conditional_vars, branch_vars))

    return result


_TARGET_SCOPE_KEYWORDS = frozenset({"PUBLIC", "PRIVATE", "INTERFACE"})


def parse_target_include_directories(cmake_text: str) -> List[str]:
    """Parse every `target_include_directories(<target> <PUBLIC|PRIVATE|
    INTERFACE> <dir> [<dir> ...] ...)` call's directory tokens, in source
    order across every such call in the file (NOT deduped). `${VAR}`
    tokens are expanded the same way `parse_paths` expands SRCS/
    INCLUDE_DIRS tokens, including branch-enumerating a conditionally
    `set()` var via `_expand_path_token` -- none of this codebase's
    `target_include_directories` calls currently reference one, but a
    future one gets the same "check every branch" treatment as SRCS,
    never a silent last-branch-wins guess.

    `${CMAKE_CURRENT_LIST_DIR}` is left unresolved here, same as
    `parse_paths` -- the caller substitutes the component's own directory.
    Returns `[]` if the file has no `target_include_directories(...)`
    call."""
    stripped = strip_cmake_comments(cmake_text)
    resolved_vars, conditional_vars = _resolve_vars(stripped)
    branch_vars = _resolve_vars_branches(stripped)

    result: List[str] = []
    for _fn_name, block in _iter_call_blocks(stripped, "target_include_directories"):
        for _current, tok in _iter_arg_tokens(block, _TARGET_SCOPE_KEYWORDS):
            result.extend(_expand_path_token(tok, resolved_vars, conditional_vars, branch_vars))
    return result


def parse_include_calls(cmake_text: str) -> List[str]:
    """Parse every `include(<file>)` call's first (and only path-bearing)
    argument, in source order across every call in the file. `${VAR}`
    tokens expand the same way `parse_target_include_directories` does
    (branch-enumerating any conditional `set()`, never a silent guess).

    A CMake `include(<name>)` argument is only sometimes a filesystem path
    relative to the caller's own directory -- it can ALSO be a bare CMake
    built-in/find-able MODULE name (e.g. `include(GNUInstallDirs)`),
    resolved via `CMAKE_MODULE_PATH`, which is a wholly different
    resolution mechanism this text-only parser has no way to evaluate.
    None of this codebase's `include(...)` calls are a bare module name
    today (every one is a same-tree `${CMAKE_CURRENT_LIST_DIR}`-relative
    `.cmake` path) -- to avoid misclassifying a future bare-module
    `include(...)` as a broken relative path, an expanded token is only
    returned (and thus checked) when it contains a `/` or `\\`; a token
    with neither is a bare module name and is excluded from this parser's
    output entirely (not a violation, not a guess -- a different, out-of-
    scope resolution mechanism)."""
    stripped = strip_cmake_comments(cmake_text)
    resolved_vars, conditional_vars = _resolve_vars(stripped)
    branch_vars = _resolve_vars_branches(stripped)

    result: List[str] = []
    for _fn_name, block in _iter_call_blocks(stripped, "include"):
        toks = block.split()
        if not toks:
            continue
        first = toks[0].strip('"')
        for clean in _expand_path_token(first, resolved_vars, conditional_vars, branch_vars):
            if "/" in clean or "\\" in clean:
                result.append(clean)
    return result


_PROPERTY_GET_COMMANDS = ("idf_component_get_property", "idf_build_get_property")
_PROPERTY_GET_RE = re.compile(
    r'\b(?:idf_component_get_property|idf_build_get_property)\s*\(\s*(\w+)\b'
)


def parse_var_assignment_events(cmake_text: str) -> Dict[str, List[Tuple[int, str]]]:
    """Return var-name -> `[(position, kind), ...]` (sorted by position,
    ties broken by original scan order — `set`/`append`/`property` never
    collide in practice since each comes from a distinct regex pass) for
    EVERY assignment-like event touching that var anywhere in
    `cmake_text`: `kind` is `"set"` (a `set(VAR ...)` call), `"append"` (a
    `list(APPEND VAR ...)` call), or `"property"` (an
    `idf_component_get_property(VAR ...)` / `idf_build_get_property(VAR
    ...)` call). Positions are offsets into the COMMENT-STRIPPED text
    (mirrors `_resolve_vars`/`_resolve_vars_branches`'s coordinate space)
    — ORDERING-only use (which event happened before which), never treated
    as exact character offsets into the original file.

    Used by `single_opaque_property_vars` to decide, per var, whether its
    ENTIRE assignment history in this file is exactly one property-get
    call — the only case eligible for the `component-path-unresolved`
    lint rule's opaque-var exemption (B1-1134 review HIGH: a flat,
    provenance-blind "was this var EVER assigned by a property call
    anywhere in the file" check silently exempted a fabricated path
    referencing a var of the same name assigned by an unrelated, later
    property-get call — this function is what makes the exemption
    order-aware and fail-closed instead).

    KNOWN LIMITATION (pre-existing, not fixed here — same root cause as
    `_iter_call_blocks`'s documented limitation, but a WORSE consequence
    for this function specifically): `_SET_RE`/`_LIST_APPEND_RE`/
    `_PROPERTY_GET_RE` all scan comment-stripped text directly, with no
    string-literal awareness. A property-get "call" spelled inside a
    quoted CMake string is indistinguishable from a real one and is
    recorded as a genuine `"property"` event, e.g.:

        message("workaround: idf_component_get_property(bogus_var "
                 "some_component COMPONENT_DIR)")
        idf_component_register(SRCS "${bogus_var}/nope.c")

    makes `bogus_var` look like it has exactly one, legitimate property-
    get assignment, which grants `single_opaque_property_vars` a real
    exemption for a SRCS path that can never resolve — a SILENT false
    exemption, not a loud violation (contrast `_iter_call_blocks`'s
    path-token consumers, where a misdetected call fails loud instead).
    Not fixed here: the trigger is contrived (no such string exists
    anywhere in this codebase today), and a correct fix needs to
    distinguish "inside a string that's itself the argument we want" from
    "inside a string that's just prose" without breaking SRCS/
    INCLUDE_DIRS token extraction (those ARE quoted strings) — tracked as
    B1-1139 ("component-path-unresolved lint: CMake scanning is not
    string-literal-aware, allowing a silent path exemption"; linked to
    B1-1134 and B1-980) rather than folded into this commit."""
    stripped = strip_cmake_comments(cmake_text)
    events: Dict[str, List[Tuple[int, str]]] = {}
    for m in _SET_RE.finditer(stripped):
        events.setdefault(m.group(1), []).append((m.start(), "set"))
    for m in _LIST_APPEND_RE.finditer(stripped):
        events.setdefault(m.group(1), []).append((m.start(), "append"))
    for m in _PROPERTY_GET_RE.finditer(stripped):
        events.setdefault(m.group(1), []).append((m.start(), "property"))
    for name in events:
        events[name].sort(key=lambda e: e[0])
    return events


def single_opaque_property_vars(cmake_text: str) -> Dict[str, int]:
    """Return var-name -> the (comment-stripped-text) position of that
    var's SOLE assignment event, restricted to vars whose ENTIRE
    assignment history in `cmake_text` is EXACTLY ONE
    `idf_component_get_property`/`idf_build_get_property` call — e.g.
    `bb_diag`'s `espcoredump_dir`, assigned once from `espcoredump`'s
    `COMPONENT_DIR` property and never `set()`/`list(APPEND)`ed anywhere
    else in the file. The property VALUE is resolved by ESP-IDF's build
    system at CMake configure time (which component search paths are
    active, where a given component's sources physically live) — not
    statically derivable from this file's own text, and out of scope for
    a text-only parser to simulate; a path token referencing one of these
    vars is therefore a MODELED, DOCUMENTED exclusion from the
    `component-path-unresolved` lint rule's existence check — not a
    violation (the rule isn't claiming the path is broken, it genuinely
    doesn't know) and not a guess (it never fabricates the var's value).

    Deliberately conservative, per B1-1134 review HIGH — a var qualifies
    ONLY when ALL of:
      1. it's assigned by an `idf_*_get_property` call, AND
      2. (enforced by the CALLER, `_opaque_var_exemption`, which compares
         this function's recorded position against the referencing
         occurrence's own position) that call precedes the reference in
         source order, AND
      3. the var has NO OTHER assignment anywhere in the file — a second
         property-get call, a `set()`, or a `list(APPEND)` for the same
         name (e.g. reused for an unrelated declaration elsewhere)
         disqualifies it ENTIRELY, even for a reference that would
         otherwise legitimately follow the property-get. A var with more
         than one assignment event is excluded from the returned dict, no
         matter what those events are — a false negative (silently
         exempting a fabricated path) is the exact defect class this rule
         exists to prevent; a spurious violation from being over-strict
         here is loud and gets fixed.

    KNOWN LIMITATION (pre-existing, not fixed here): this function is
    BLOCK-STRUCTURE-BLIND, same general limitation as
    `_resolve_vars_branches` — a property-get call inside an `if()`
    branch is recorded as a single unconditional event by
    `parse_var_assignment_events`, even though it may not actually
    execute at CMake configure time (the branch might not be taken). It
    is NOT branch-enumerated the way a conditionally-`set()` path var is
    (`_resolve_vars_branches`/`_expand_path_token`) — there is currently
    no real-tree component whose `idf_*_get_property` call is itself
    inside an `if()`, so this hasn't been observed to matter in practice.
    Recorded here rather than fixed."""
    events = parse_var_assignment_events(cmake_text)
    result: Dict[str, int] = {}
    for name, evs in events.items():
        if len(evs) == 1 and evs[0][1] == "property":
            result[name] = evs[0][0]
    return result


_EMBED_ASSETS_KEYWORDS = frozenset({"OUT_SRCS", "ASSETS"})


def parse_embed_assets(cmake_text: str) -> Dict[str, List[str]]:
    """Parse every `bb_embed_assets(OUT_SRCS <var> ASSETS <file>:<symbol>
    [...])` call in `cmake_text` (see `cmake/bbtool.cmake`) -- the CMake
    macro `bb_prov_default_form`/similar components use to gzip-embed a
    static asset (e.g. an HTML form) into a generated `.c` byte-array
    source AT CMAKE CONFIGURE TIME, appending the generated path to
    `OUT_SRCS`'s var in the caller's scope.

    Returns `{out_var_name: [asset_file, ...]}` -- the `OUT_SRCS` var name
    mapped to its `ASSETS` entries' file part (the `:<symbol>` suffix
    stripped). This turns an otherwise-unresolvable `${OUT_SRCS_var}` SRCS
    token into a MODELED one: the component-path-unresolved lint rule
    recognizes any SRCS token matching a key here as "generated by
    bb_embed_assets, not a literal on-disk path to check", and instead
    validates every one of THIS function's returned asset-file entries
    exists (resolved relative to the component's own directory, mirroring
    `bb_embed_assets`'s own documented "input paths resolve relative to
    CMAKE_CURRENT_LIST_DIR" contract) -- so a renamed/deleted asset input
    still fails the guard; this is strictly more coverage than the
    all-or-nothing unresolved-variable failure it replaces, never an
    exemption.

    `${VAR}` tokens (OUT_SRCS's var name, or an ASSETS file/symbol token)
    are expanded via a preceding `set()`/`list(APPEND)` the same way
    `parse_paths` expands non-conditional path tokens. No conditional/
    branch handling is needed here (`bb_embed_assets(...)` calls aren't
    observed inside `if()` blocks in this codebase); if a future one is,
    its ASSETS entries still get validated on every textual occurrence of
    the call, which is a safe over-approximation, not an exemption.

    Returns `{}` if the file has no `bb_embed_assets(...)` call. Multiple
    calls sharing one `OUT_SRCS` var accumulate their ASSETS entries onto
    that same key."""
    stripped = strip_cmake_comments(cmake_text)
    resolved_vars, _conditional_vars = _resolve_vars(stripped)

    result: Dict[str, List[str]] = {}
    for _fn_name, block in _iter_call_blocks(stripped, "bb_embed_assets"):
        out_var: Optional[str] = None
        assets: List[str] = []
        for current, tok in _iter_arg_tokens(block, _EMBED_ASSETS_KEYWORDS):
            expanded = [t.strip('"') for t in _expand_token(tok, resolved_vars)]
            if current == "OUT_SRCS":
                for e in expanded:
                    out_var = e
            elif current == "ASSETS":
                for e in expanded:
                    assets.append(e.split(":", 1)[0])
        if out_var is not None:
            result.setdefault(out_var, []).extend(assets)
    return result


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
