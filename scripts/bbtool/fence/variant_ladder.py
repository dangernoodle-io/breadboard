"""variant_ladder fence family — stops NEW variant-ladder public APIs
(B1-1461).

Freezes breadboard's "no variant ladders" convention (wiki
Conventions.md#api-conventions, "Convention: no variant ladders") as a
ratchet-fence guardrail: a public API gets ONE entry point; extension
happens by adding fields to a config struct, never by adding a
differently-suffixed sibling function. Epic B1-1434 removed every known
ladder in the tree and verified zero live variant-ladder declarations
remain in `components/*/include/*.h` — so this family's baseline starts
**empty**, and this is a pure "no new ladders" gate, not a shrink-only
list of pre-existing exceptions like `di_legacy`/`clamp`/`sat_sub`.

Marker type scanned:
  - `variant_ladder_suffix` — a public function declaration in
    `components/<name>/include/*.h` whose name carries one of a fixed set
    of ladder-shaped suffixes (`_ex`, `_ex2`, `_ex3`, ..., `_named`,
    `_with_*`, `_handle`, `_ref`, `_sized`, `_full`) AND whose un-suffixed
    BASE name is ALSO declared public in the same component's header set.
    Both rungs must exist as public entry points for this to be a ladder
    — a lone `_handle`/`_ex`-suffixed function with no base sibling is not
    flagged (see the worked example below).

THE DETECTOR RULE, PLAIN WORDS: a suffixed function `bb_foo_bar_ex()` is
only a violation if `bb_foo_bar()` is ALSO declared public in the same
component's `include/*.h` set. That pairing — two public entry points
where one is just the other with a suffix and (implicitly) a wider
signature — is what makes it a ladder, not the suffix spelling alone.

**Acceptance case (must NOT fire): `bb_http_server_get_handle()`.**
`components/bb_http_server/include/bb_http_server.h` declares
`bb_http_server_get_handle()` but there is no `bb_http_server_get()`
sibling anywhere in `bb_http_server`'s public headers — the `_handle`
suffix here is incidental (see the wiki's own worked example), so the
base-sibling check correctly leaves it unflagged. `test_fence_variant_ladder.py`
exercises this exact shape as a synthetic fixture.

**Not flagged: typed accessor families.** `_u8`/`_u16`/`_u32`/`_i8`/
`_i16`/`_i32`/`_i64`/`_u64`/`_f32`/`_f64`/`_bool`-suffixed siblings are
legitimate type overloading (C has no generics), never a variant ladder —
excluded via `_TYPED_ACCESSOR_RE` before any ladder-suffix pattern is even
tried, so a fixed-width-type name can never be mistaken for e.g. a
`_sized`/`_ref` ladder suffix.

**Scope: public API only.** Only `components/*/include/*.h` declarations
are scanned — a function declared `static`/`inline` (or matched by any of
`_FN_DECL_SKIP_KW`) never binds the "no variant ladders" convention in the
first place, and internal `.c`-file helpers are outside this family's scan
roots entirely (mirrors `bb-prefix`'s own column-0, single-line
declaration heuristic in `commands/lint.py`).

**THE CEILING, STATED PLAINLY — read this before trusting a green run.**
This is a cheap regex scan, not a parser, and a green result must NOT be
read as full compliance with the no-variant-ladders convention. B1-558 is
the standing human sweep that covers everything below; this fence only
ever stops the ladder shape from growing back through the ONE gap a
column-0, single-line regex can reliably see. The real gaps:

  (a) **Multi-line declarations.** The scan is column-0, single-line only
      (same heuristic as `bb-prefix` in `commands/lint.py`) — a return
      type wrapped onto its own line above the function name (or a
      declaration split across lines for any other reason) is never
      matched, ladder or not.

  (b) **Suffixes outside the fixed list.** `_LADDER_SUFFIX_PATTERNS` is a
      fixed, hand-maintained set (`_ex`/`_ex\\d*`, `_named`, `_with_*`,
      `_handle`, `_ref`, `_sized`, `_full`). A ladder sibling spelled with
      any other suffix — `_opts`, `_v2`, `_advanced`, or any future
      spelling not yet added to the list — is invisible to this fence
      until someone extends the pattern set.

  (c) **Un-suffixed thin wrappers — the case the convention actually
      defines.** The convention bans the SHAPE: a base call and a wider
      sibling coexisting as two public entry points — not the suffix
      spelling. An UN-SUFFIXED thin wrapper (two public functions sharing
      a name prefix where one merely forwards into the other with extra
      parameters, e.g. a hypothetical `bb_foo_configure(...)` that just
      calls `bb_foo_configure_impl(...)` with a default appended) carries
      no suffix at all, so it is PERMANENTLY invisible to any suffix
      scanner, this fence included — no amount of extending (b)'s pattern
      list ever closes this gap.

  (d) **A `(` appearing before the declared function's own name.** The
      recognizer regex (`_FN_DECL_RE`) requires the function name to sit
      IMMEDIATELY before the opening `(` of its parameter list; any other
      `(` earlier on the line (from something the regex can't parse as a
      return-type word) makes the whole line fail to match, declaration or
      not. Two known shapes fall in this gap, both verified against the
      real scanner on synthetic fixtures and both LATENT (zero occurrences
      in this tree today, so no baseline entry masks them):
        - a function returning a function pointer, e.g.
          `int (*bb_foo_get(void))(int);` — this codebase typedefs
          callback types instead of writing this shape inline, but nothing
          stops it from being written this way.
        - a LEADING call-style macro/attribute before the return type,
          e.g. `__attribute__((deprecated)) bb_err_t bb_foo_get(void);` —
          a TRAILING `__attribute__((...))` (after the `)` that closes the
          function's own parameter list) does not hit this gap and matches
          normally; a bare macro prefix with no parens of its own (e.g.
          `BB_EXPORT bb_err_t bb_foo_get(void);`) also matches normally —
          only a *leading* macro that itself carries `(...)` is affected.
"""
from __future__ import annotations
import re
import sys
from pathlib import Path
from typing import Dict, List, Set, Tuple

from fence import _base
from fence._base import Marker

_SCAN_ROOTS = ("components",)
_HEADER_GLOBS = ["*/include/*.h"]

_BUCKETS = {
    "variant_ladder_suffix": "suffixed variant-ladder sibling",
}


def _bucket_for(marker_type: str) -> str:
    return _BUCKETS.get(marker_type, marker_type)


def counts_by_bucket(markers: Set[Marker]) -> dict:
    return _base.counts_by_bucket(markers, bucket_fn=_bucket_for)


# ---------------------------------------------------------------------------
# Public function-declaration recognition — same column-0, single-line
# heuristic as commands/lint.py's `_BB_PREFIX_FN_RE`/`_BB_PREFIX_SKIP_KW`
# (deliberately not re-imported: this family stays self-contained, same
# convention as di_legacy.py/log_secret.py). A `static`/`inline` (or other
# skip-keyword-carrying) declaration is never public API and is excluded.
# ---------------------------------------------------------------------------

_FN_DECL_RE = re.compile(
    r'^([A-Za-z_]\w*(?:\s*\*)?(?:\s+[A-Za-z_]\w*(?:\s*\*)?)*(?:\s*\*\s*|\s+))'
    # return type: first word, then zero-or-more further words, each word
    # optionally carrying a trailing `*` (so a pointer star can attach to
    # ANY prefix word — `void *`, `bb_foo_t *`, `const bb_foo_t *`,
    # `unsigned char *` — not only the first), followed by a MANDATORY
    # separator before the function name: either a bare `*` (with optional
    # surrounding whitespace, so the star may sit directly against the
    # name — `bb_foo_t *bb_foo_get(...)`) or plain whitespace (so
    # `bb_err_t bb_foo_get(...)` still matches, unchanged).
    r'([a-z][a-zA-Z0-9_]*)'                       # function name (lowercase-start)
    r'\s*\('                                       # opening paren
)
_FN_DECL_SKIP_KW = frozenset({
    'static', 'inline', 'extern', 'typedef',
    'if', 'while', 'for', 'return',
})
# NOTE: `const`/`volatile` are deliberately NOT skip keywords — they are
# legitimate return-type qualifiers on real public declarations (e.g.
# `const bb_foo_t *bb_foo_get(void);`, part of finding 1's required
# pointer-return coverage), not a signal the line isn't a function
# declaration. `register` is a storage-class specifier that (like
# `const`/`volatile`) never disqualifies the line either; dropped for the
# same reason rather than left in as a stale, functionally-inert entry.

# `struct`/`enum`/`union` are handled SEPARATELY from the plain skip-keyword
# set above (third-round finding 1): they are ambiguous, unlike
# `static`/`typedef`/etc, because they can appear either (a) introducing a
# TYPE DEFINITION/forward-decl (`struct bb_foo {`, `struct bb_foo;`,
# `enum bb_state { A, B };` — never a public function, must be skipped) or
# (b) as an ordinary RETURN-TYPE QUALIFIER on a real declaration
# (`struct bb_foo *bb_foo_get(void);`, `enum bb_state bb_state_get(void);`
# — a legitimate public accessor, must MATCH). A bare keyword-presence
# check (the prior implementation) cannot tell these apart and blinded the
# scan to every opaque-struct-pointer/enum-return accessor — an ordinary C
# idiom, not a hypothetical. Disambiguated structurally in
# `_is_type_definition_line` instead of by keyword alone.
_FN_DECL_TYPE_KW = frozenset({'struct', 'enum', 'union'})


def _is_type_definition_line(stripped: str) -> bool:
    """True when `stripped` (a line whose prefix carries `struct`/`enum`/
    `union`) is a type DEFINITION or forward-declaration rather than a
    function declaration merely using one of those keywords as a
    return-type qualifier. Distinguished structurally: a type-keyword line
    disqualifies only when there is no `(` at all (`struct bb_foo;`,
    `struct bb_foo {`, `enum bb_state { A, B };` — none of these declare a
    function), or when a `{` appears before the first `(` (an inline
    definition body that happens to embed parens elsewhere on the line,
    e.g. function-pointer struct members). A `(` appearing before any `{`
    (or with no `{` at all) is what every real function declaration looks
    like — `struct bb_foo *bb_foo_get(void);`,
    `enum bb_state bb_state_get(void);` — so those are left unflagged and
    fall through to a normal declaration match."""
    paren = stripped.find('(')
    if paren == -1:
        return True
    brace = stripped.find('{')
    return brace != -1 and brace < paren


def _declared_public_fns(root: Path) -> Dict[str, List[Tuple[str, str, int]]]:
    """Component name -> list of (fn_name, rel_path, lineno) for every
    public function DECLARATION found in that component's `include/*.h`
    headers."""
    by_component: Dict[str, List[Tuple[str, str, int]]] = {}
    for path in _base.iter_files(root, _SCAN_ROOTS, _HEADER_GLOBS):
        rel = _base.rel(root, path)
        parts = Path(rel).parts
        if len(parts) < 3 or parts[0] != "components":
            continue
        component = parts[1]
        for i, line in enumerate(_base.read(path).splitlines(), 1):
            stripped = line.strip()
            if not stripped or _base.is_noise_line(stripped):
                continue
            m = _FN_DECL_RE.match(line)
            if not m:
                continue
            prefix_words = m.group(1).split()
            if any(w in _FN_DECL_SKIP_KW for w in prefix_words):
                continue
            if any(w in _FN_DECL_TYPE_KW for w in prefix_words) and _is_type_definition_line(stripped):
                continue
            name = m.group(2)
            by_component.setdefault(component, []).append((name, rel, i))
    return by_component


# ---------------------------------------------------------------------------
# Ladder-suffix recognition. Typed accessor families are excluded FIRST so
# they can never be mistaken for a ladder suffix by any pattern below.
# ---------------------------------------------------------------------------

_TYPED_ACCESSOR_RE = re.compile(
    r'_(?:u8|u16|u32|u64|i8|i16|i32|i64|f32|f64|bool)$'
)

_LADDER_SUFFIX_PATTERNS = [
    re.compile(r'_ex\d*$'),            # _ex, _ex2, _ex3, ...
    re.compile(r'_named$'),
    re.compile(r'_with_[A-Za-z0-9_]+$'),
    re.compile(r'_handle$'),
    re.compile(r'_ref$'),
    re.compile(r'_sized$'),
    re.compile(r'_full$'),
]


def _ladder_base_name(name: str) -> str:
    """Returns the stripped base name if `name` carries a ladder-shaped
    suffix, else "" (falsy). A typed-accessor-suffixed name never reaches
    the suffix-pattern loop at all."""
    if _TYPED_ACCESSOR_RE.search(name):
        return ""
    for pattern in _LADDER_SUFFIX_PATTERNS:
        m = pattern.search(name)
        if m:
            base = name[: m.start()]
            if base:
                return base
    return ""


def _scan_variant_ladder_suffix(root: Path) -> Set[Marker]:
    found: Set[Marker] = set()
    by_component = _declared_public_fns(root)

    for component, decls in by_component.items():
        names = {n for n, _rel, _line in decls}
        for name, rel, _line in decls:
            base = _ladder_base_name(name)
            # `base == name` is unreachable here: `_ladder_base_name` only
            # ever returns a truthy value by slicing `name` up to a matched
            # suffix pattern's start, which is always a strict prefix of
            # `name` (every suffix pattern below matches at least one
            # character). Kept as `not base` only.
            if not base:
                continue
            if base in names:
                found.add(
                    Marker("variant_ladder_suffix", rel, f"{component}:{base}->{name}")
                )

    print(
        "INFO [fence:variant_ladder]: this fence catches SUFFIXED variant"
        " ladders only (base + a suffixed sibling both public) — an"
        " UN-SUFFIXED thin wrapper forwarding into a wider sibling is"
        " invisible to a suffix scanner and this fence will never catch it."
        " A green result is NOT full compliance with the no-variant-ladders"
        " convention; B1-558 is the standing human sweep that covers that"
        " gap.",
        file=sys.stderr,
    )
    return found


def identity(m: Marker):
    """Ratchet-diff identity key: (marker_type, symbol_identifier) — same
    convention as every other family (see di_legacy.identity). The id
    already embeds the owning component, so a bare file rename within the
    same component is a no-op diff, not a spurious remove+add.

    Byte-identical to `_base.default_identity` — declared here anyway, same
    as every other family (e.g. di_legacy.identity), for per-family
    convention/clarity: `_base.identity_fn_for` looks up `identity` on the
    module by name, so each family owning an explicit definition keeps the
    lookup uniform and doesn't rely on a silent fallback."""
    return (m.type, m.id)


def scan_all(root: str) -> Set[Marker]:
    return _base.scan_all(sys.modules[__name__], root)
