"""wire — library module (decision #735) for the codegen that REALIZES
AUTOWIRE mode. Folded into the `commands.codegen` CLI command; this module
now only exports the shared marker-collection + rendering functions
`codegen.py` (and its tests) call into.

Reuses `composition.resolve_composition()` — the same composition closure
walk used for the CMake REQUIRES fragment — as the SSOT for "which
components are in this build". Over that component set, it:

  1. Greps each component's public header(s) for `// bbtool:init` markers
     (wire_parse.parse_markers).
  2. Topo-sorts them per tier (wire_graph.topo_sort).
  3. Emits a GITIGNORED `bb_app_init.c` (decision #725 — generated code is a
     build artifact, never committed) defining:
       - `bb_app_init_early(void)`  — the `early` tier.
       - `bb_app_init_rest(void)`   — `pre_http` tier, then (iff an
         `http_server`-providing entry is present) the server-start line,
         then `regular` tier (server=true entries receive the handle).
       - `bb_app_init(void)`        — both combined, in order.
     Plus a sibling `.cmake` fragment naming the generated `.c` so a
     consumer's CMakeLists can pull it into SRCS without hand-listing it.

Conventions (spelled out here since decision #735 leaves fn signatures to
the marker author, validated only by the C compiler at build time):
  - Every tier entry's `fn` is called with zero arguments and is expected to
    return `bb_err_t`, EXCEPT the one entry (if any) whose `provides` list
    contains the reserved key `http_server` — that entry's `fn` is called
    with zero arguments and is expected to return the http handle type
    directly (captured via `__auto_type` so the codegen never needs to know
    the concrete handle typedef). **Exactly one** such entry is permitted,
    and it MUST be tier=pre_http — see `render_source`'s WireError checks
    below. A malformed `// bbtool:init` marker set (two components both
    marking `provides=http_server`, or one marking it outside `pre_http`)
    is caught here rather than silently emitting a double call.
  - Every `server=true` entry (always tier=regular) is called with exactly
    one argument: the captured http handle. It still returns `bb_err_t`.
  - First-error semantics per tier: the first non-BB_OK return is recorded
    and returned, but every remaining entry in that tier still runs.

**Known limitation:** if the http_server-providing `fn` itself returns NULL
(or an otherwise "no handle" sentinel), `render_source` has no way to detect
that at codegen time — the generated `__auto_type` capture and every
downstream `server=true` call proceed unconditionally. This is an accepted
limitation of grep-time codegen (no type information, no runtime check
inserted); a NULL http handle surfaces as a runtime crash/assert in the
`server=true` consumer instead of a codegen-time error.

Setter-injection (`consumes=`/`// bbtool:provides`) path — a SECOND, parallel
emission mechanism, deliberately not folded into the http_server path above:
an entry marked `consumes=<key>` is a void-returning setter; if a `//
bbtool:provides key=<key> symbol=<symbol>` declaration for that key is also
present in the resolved composition, `render_source` emits a plain
`{fn}({symbol});` call (no `bb_err_t`/`bb_app_rc` wrapper — setters return
void) in place of that entry's normal tier call. If no matching provider is
in the resolved set, the entry is silently dropped — this conditionality
(present iff both provider and consumer are composed) is the whole point,
mirrors `requires=`'s edge concept but with soft- rather than hard-failure
semantics, and adds no new tier/phase.

DEFERRED (do not implement here): a PlatformIO pre-build hook wiring this in
automatically; a lint rule that validates marker hygiene.
"""
from __future__ import annotations
import glob
import os
from typing import Dict, List, Tuple

from discovery import build_index, normalize_roots
from wire_parse import InitEntry, ProvidesEntry, parse_markers, parse_provides_markers

DEFAULT_OUT_REL = os.path.join("main", "generated", "bb_app_init.c")

HTTP_SERVER_PROVIDES_KEY = "http_server"


class WireError(Exception):
    """Hard error for wire-invariant violations (e.g. a server=true entry
    with no http_server provider in the set, or more than one / a
    mis-tiered http_server provider)."""


# ---------------------------------------------------------------------------
# Marker collection
# ---------------------------------------------------------------------------

def _component_headers(roots, name: str, platform: str) -> List[Tuple[str, str]]:
    """Public header files for one component — looked up via the discovery
    index (B1-979) rather than a hand-rolled path-position encoding; mirrors
    boards.derive_component's directory convention: components/<name>/
    include/*.h, falling back to components/<name>/*.h (flat layout), plus
    platform/<platform>/<name>/ (include/ or flat), sorted for determinism.

    `roots` is a single root (str, back-compat) or an ordered list of roots
    (B1-1084). Returns `(owning_root, rel_header)` pairs — `rel_header` is
    relative to `name`'s OWNING root (`index.entry(name).root`), never
    blindly to `roots[0]`, so a component discovered under a non-primary
    root grep-resolves correctly instead of raising/misresolving against the
    wrong tree."""
    roots_list = normalize_roots(roots)
    index = build_index(roots_list)
    entry = index.entry(name)
    owning_root = entry.root if entry is not None else (roots_list[0] if roots_list else "")
    headers: List[str] = []

    comp_dir = index.component_dir(name)
    if comp_dir is not None:
        comp_include = comp_dir / "include"
        if comp_include.is_dir():
            headers.extend(sorted(glob.glob(os.path.join(str(comp_include), "*.h"))))
        else:
            headers.extend(sorted(glob.glob(os.path.join(str(comp_dir), "*.h"))))

    plat_dir = index.platform_dir(name, platform)
    if plat_dir is not None:
        plat_include = plat_dir / "include"
        if plat_include.is_dir():
            headers.extend(sorted(glob.glob(os.path.join(str(plat_include), "*.h"))))
        else:
            headers.extend(sorted(glob.glob(os.path.join(str(plat_dir), "*.h"))))

    return [(owning_root, os.path.relpath(h, owning_root).replace(os.sep, "/")) for h in headers]


def _src_file_repr(entry_root: str, primary_root: str, rel_header: str) -> str:
    """B1-1084 Fork 2: the `src_file` recorded on each parsed marker (feeds
    generated `bb_app_init.c` comments + `bbtool codegen` stdout, e.g.
    `[{tier}] {fn} ({src_file}:{line})`) must stay unambiguous across roots.
    A component under the PRIMARY root (`roots[0]`) keeps the plain
    repo-root-relative path — byte-identical to pre-B1-1084 output, since a
    single-root call always has `entry_root == primary_root`. A component
    under a NON-primary root gets the full absolute path instead of a bare
    relative one, which could otherwise collide (visually, in a human-read
    log) with an identically-shaped relative path under a different root."""
    if entry_root == primary_root:
        return rel_header
    return os.path.join(entry_root, rel_header).replace(os.sep, "/")


def collect_entries(roots, components: List[str], platform: str) -> List[InitEntry]:
    """Grep every resolved component's public header(s), in composition
    order (dependency-before-dependent, as returned by resolve_composition),
    then header-path order, then in-file line order — this fixed walk order
    is the "parse order" wire_graph.topo_sort tie-breaks on.

    `roots` is a single root (str, back-compat) or an ordered list of roots
    (B1-1084) — see `_component_headers`/`_src_file_repr` for the owning-root
    resolution and `src_file` representation this threads through."""
    roots_list = normalize_roots(roots)
    primary_root = roots_list[0] if roots_list else ""
    entries: List[InitEntry] = []
    for name in components:
        for entry_root, rel_header in _component_headers(roots_list, name, platform):
            abs_header = os.path.join(entry_root, rel_header)
            with open(abs_header, encoding="utf-8") as f:
                text = f.read()
            src_file = _src_file_repr(entry_root, primary_root, rel_header)
            entries.extend(parse_markers(text, src_file=src_file))
    return entries


def collect_provides_entries(roots, components: List[str], platform: str) -> List[ProvidesEntry]:
    """Grep every resolved component's public header(s) for `// bbtool:provides`
    declaration markers, same walk order as `collect_entries` (irrelevant here
    since these are unordered key->symbol declarations, never tier-sorted).
    A SECOND, parallel collector alongside `collect_entries` — never merged
    into its InitEntry return, since these records never enter the tier
    loop. `roots` is a single root (str, back-compat) or an ordered list of
    roots (B1-1084) — same owning-root resolution as `collect_entries`."""
    roots_list = normalize_roots(roots)
    primary_root = roots_list[0] if roots_list else ""
    entries: List[ProvidesEntry] = []
    for name in components:
        for entry_root, rel_header in _component_headers(roots_list, name, platform):
            abs_header = os.path.join(entry_root, rel_header)
            with open(abs_header, encoding="utf-8") as f:
                text = f.read()
            src_file = _src_file_repr(entry_root, primary_root, rel_header)
            entries.extend(parse_provides_markers(text, src_file=src_file))
    return entries


# ---------------------------------------------------------------------------
# Rendering
# ---------------------------------------------------------------------------

def _headers_for(entries: List[InitEntry]) -> List[str]:
    return sorted({e.src_file for e in entries})


def _http_server_providers(entries: List[InitEntry]) -> List[InitEntry]:
    return [e for e in entries if HTTP_SERVER_PROVIDES_KEY in e.provides]


def _requires_guard_cond(entry: InitEntry) -> str:
    return " && ".join(f"bb_app_avail_{tok}" for tok in entry.requires)


def _indent_block(body: str) -> str:
    """Re-indent an already-rendered call block one level deeper (4 spaces),
    leaving blank lines untouched -- used when wrapping a call in an
    availability `if` guard."""
    return "".join(("    " + line if line.strip() else line) for line in body.splitlines(keepends=True))


def _guard_requires(entry: InitEntry, body: str) -> str:
    """Wrap `body` (an already-rendered, 4-space-indented call block) in a
    `requires=` availability guard (B1-853): only run the call if every
    required token's provider has already succeeded; otherwise SKIP it (never
    call it) and log a WARN, rather than letting a dependent run against a
    failed/skipped provider. `entry.requires` empty -> `body` returned
    UNCHANGED (byte-identical to the pre-gating call for entries with no
    `requires=`)."""
    if not entry.requires:
        return body
    cond = _requires_guard_cond(entry)
    return (
        f"    if ({cond}) {{\n"
        f"{_indent_block(body)}"
        f"    }} else {{\n"
        f'        bb_log_w(BB_APP_INIT_TAG, "skipping {entry.fn}: required provider unavailable");\n'
        f"    }}\n"
    )


def _emit_provides_avail(entry: InitEntry, guarded_tokens: frozenset, success_expr: str = None) -> str:
    """After `entry`'s call, mark every token it `provides=` (that some OTHER
    entry actually gates on via `requires=`, i.e. is in `guarded_tokens`) as
    available. `success_expr` is the runtime condition under which the entry
    counts as having succeeded -- `"bb_app_rc == BB_OK"` for a normal
    `bb_err_t` call; `None` for a void `consumes=` setter or the http-handle
    capture line, both of which report no `bb_err_t` and are treated as
    always-succeeding once called (mirrors their existing unconditional
    treatment elsewhere in this module). Multi-provider tokens: availability
    is the OR of every provider's success, since each provider only ever sets
    its flag to `true`, never resets it to `false`."""
    tokens = [t for t in entry.provides if t in guarded_tokens]
    if not tokens:
        return ""
    sets = "".join(f"    bb_app_avail_{t} = true;\n" for t in tokens)
    if success_expr is None:
        return sets
    return f"    if ({success_expr}) {{\n{_indent_block(sets)}    }}\n"


def _emit_call(entry: InitEntry, guarded_tokens: frozenset = frozenset(), handle_var: str = None) -> str:
    arg = handle_var if (entry.server and handle_var) else ""
    body = (
        f"    bb_app_rc = {entry.fn}({arg});\n"
        f"    if (bb_app_rc != BB_OK && bb_app_first_err == BB_OK) {{ "
        f"bb_app_first_err = bb_app_rc; }}\n"
    )
    body += _emit_provides_avail(entry, guarded_tokens, success_expr="bb_app_rc == BB_OK")
    return _guard_requires(entry, body)


def _value_providers(provides_records: List[ProvidesEntry]) -> Dict[str, str]:
    """key -> symbol map from `// bbtool:provides` declarations. No closure
    filter is applied here: `provides_records` (from `collect_provides_entries`)
    only ever contains records grepped from already-resolved-composition
    headers, so every record is already guaranteed to be in the closure.

    Raises WireError if two records in the resolved set declare the same
    `key` — mirrors the http_server path's "at most one provider" check
    above: silently last-wins would let a stale/duplicate marker wire the
    wrong setter, surfacing only as a runtime mismatch. This holds even for
    an identical key+identical symbol pair — a duplicate declaration is
    still a hard error, not deduplicated, since the simplest correct
    behavior is to always reject ambiguity rather than special-case an
    accidental duplicate as harmless."""
    value_providers: Dict[str, str] = {}
    declared_by: Dict[str, ProvidesEntry] = {}
    for rec in provides_records:
        existing = declared_by.get(rec.key)
        if existing is not None:
            raise WireError(
                f"more than one '// bbtool:provides' declares key '{rec.key}' "
                f"({existing.src_file}:{existing.src_line} symbol={existing.symbol}, "
                f"{rec.src_file}:{rec.src_line} symbol={rec.symbol}) — exactly one "
                f"is required"
            )
        declared_by[rec.key] = rec
        value_providers[rec.key] = rec.symbol
    return value_providers


def _emit_consumes_call(entry: InitEntry, value_providers: Dict[str, str],
                         guarded_tokens: frozenset = frozenset()) -> str:
    """Setter-injection emission for an entry with `consumes` set: a plain
    void-shaped `{fn}({symbol}, {ctx});` call (never the `bb_err_t`/`bb_app_rc`
    wrapper `_emit_call` uses — setters return void). Empty string (soft-skip,
    never an error) if no matching `// bbtool:provides` declaration is in the
    resolved composition.

    `ctx` (B1-1045 PR-1) is the marker's `ctx=<expr>` when present, else the
    literal `NULL` — every `consumes=` setter now takes (symbol, ctx), so a
    marker with no `ctx=` still emits a syntactically/semantically complete
    call (a NULL ctx the provider symbol ignores)."""
    symbol = value_providers.get(entry.consumes)
    if symbol is None:
        return ""
    ctx_expr = entry.ctx or "NULL"
    body = f"    {entry.fn}({symbol}, {ctx_expr});\n"
    body += _emit_provides_avail(entry, guarded_tokens, success_expr=None)
    return _guard_requires(entry, body)


def _emit_entry(entry: InitEntry, value_providers: Dict[str, str],
                 guarded_tokens: frozenset = frozenset(), handle_var: str = None) -> str:
    """Dispatch a single tier entry to its emission path: `consumes=` entries
    go through the setter-injection path (never `_emit_call`'s bb_err_t
    convention); every other entry is unaffected, byte-for-byte the same as
    before this path existed."""
    if entry.consumes:
        return _emit_consumes_call(entry, value_providers, guarded_tokens)
    return _emit_call(entry, guarded_tokens, handle_var)


def render_source(ordered: List[InitEntry], provides_entries: List[ProvidesEntry] = ()) -> str:
    """Render bb_app_init.c from a fully tier-ordered entry list (as returned
    by wire_graph.topo_sort). `provides_entries` (from
    `collect_provides_entries`) feeds the SECOND, parallel setter-injection
    emission path (see module docstring): any entry with `consumes` set is
    dispatched through `_emit_entry` to `_emit_consumes_call` instead of the
    normal `_emit_call` bb_err_t convention — present iff a matching `//
    bbtool:provides` declaration is in `provides_entries`, silently dropped
    otherwise. Defaults to `()` (no consumes entries resolve), so callers
    that don't pass it get byte-identical output to before this path
    existed.

    Raises WireError if:
      - more than one entry in the resolved set provides `http_server`
        (would otherwise silently emit the provider call twice — once as
        the captured server-start line, once again as a plain tier call for
        every provider past the first); or
      - the (single) http_server-providing entry's tier is not `pre_http`
        (a provider outside pre_http is never excluded from its own tier's
        plain-call loop, so it would ALSO be called a second time, and the
        server handle would be captured too early/late relative to the
        tiers that need it); or
      - any server=true entry exists without an http_server provider in the
        resolved set at all; or
      - the http_server-providing entry itself has a non-empty `requires=`
        (B1-853 — its `__auto_type` handle-capture line can never be
        conditionally skipped while still producing the typed handle every
        `server=true` entry downstream depends on, so gating it is not a
        supported combination — see the check below).
    """
    early = [e for e in ordered if e.tier == "early"]
    pre_http = [e for e in ordered if e.tier == "pre_http"]
    regular = [e for e in ordered if e.tier == "regular"]

    providers = _http_server_providers(ordered)
    if len(providers) > 1:
        offenders = ", ".join(f"{e.fn} ({e.src_file}:{e.src_line})" for e in providers)
        raise WireError(
            f"more than one entry provides '{HTTP_SERVER_PROVIDES_KEY}' "
            f"({offenders}) — exactly one is required"
        )

    server_entry = providers[0] if providers else None
    has_server = server_entry is not None
    if has_server and server_entry.tier != "pre_http":
        raise WireError(
            f"{server_entry.src_file}:{server_entry.src_line}: fn={server_entry.fn} "
            f"provides '{HTTP_SERVER_PROVIDES_KEY}' but has tier='{server_entry.tier}' "
            f"(must be tier=pre_http)"
        )

    server_users = [e for e in regular if e.server]
    if server_users and not has_server:
        offenders = ", ".join(f"{e.fn} ({e.src_file}:{e.src_line})" for e in server_users)
        raise WireError(
            f"server=true entries present ({offenders}) but no entry in the "
            f"resolved set provides '{HTTP_SERVER_PROVIDES_KEY}'"
        )

    if has_server and server_entry.requires:
        raise WireError(
            f"{server_entry.src_file}:{server_entry.src_line}: fn={server_entry.fn} "
            f"provides '{HTTP_SERVER_PROVIDES_KEY}' but also declares "
            f"requires={','.join(server_entry.requires)} -- a server-handle-capture "
            f"entry's __auto_type line cannot be conditionally skipped while still "
            f"producing the typed handle every server=true entry depends on "
            f"(B1-853); remove requires= from this marker"
        )

    value_providers = _value_providers(list(provides_entries))

    # B1-853: every token some entry `requires=` -- these are the only tokens
    # that get a runtime availability flag + guard; a `provides=` token no one
    # requires never gets a flag (avoids dead/unused-but-set state). Since
    # `guarded_tokens` is exactly the union of `.requires` across `ordered`,
    # `entry.requires` is always a subset of it -- `_guard_requires` need only
    # check "is `entry.requires` non-empty", never token membership.
    guarded_tokens = frozenset(tok for e in ordered for tok in e.requires)

    headers = _headers_for(ordered)
    include_lines = "\n".join(f'#include "{h.split("/")[-1]}"' for h in headers)

    header_lines: List[str] = [
        "/* Generated by `bbtool codegen` -- DO NOT EDIT, DO NOT COMMIT.",
        " * Regenerated every build (decision #725); source of truth is the",
        " * `// bbtool:init` markers this file was grepped from (decision #735).",
        " */",
        include_lines,
    ]
    if guarded_tokens:
        gating_includes = ['#include <stdbool.h>']
        if not any(h.split("/")[-1] == "bb_log.h" for h in headers):
            gating_includes.append('#include "bb_log.h"')
        header_lines.append("\n".join(gating_includes))
        avail_block = [
            "",
            "/* B1-853: requires=/provides= runtime gating -- a token becomes",
            " * available once some provides= entry for it returns BB_OK; a",
            " * requires= entry with an unavailable token is SKIPPED (not",
            " * called), so a dependent never runs against a failed/skipped",
            " * provider. Declared at file scope so availability persists",
            " * across bb_app_init_early()/bb_app_init_rest(). */",
            '#define BB_APP_INIT_TAG "bb_app_init"',
        ]
        avail_block.extend(
            f"static bool bb_app_avail_{tok} = false;" for tok in sorted(guarded_tokens)
        )
        header_lines.append("\n".join(avail_block))

    lines: List[str] = list(header_lines) + [
        "",
        "bb_err_t bb_app_init_early(void)",
        "{",
        "    bb_err_t bb_app_first_err = BB_OK;",
        "    bb_err_t bb_app_rc;",
        "",
    ]
    for e in early:
        text = _emit_entry(e, value_providers, guarded_tokens)
        if text:
            lines.append(text)
    lines += ["    return bb_app_first_err;", "}", "", "bb_err_t bb_app_init_rest(void)", "{"]
    lines += ["    bb_err_t bb_app_first_err = BB_OK;", "    bb_err_t bb_app_rc;", ""]

    pre_http_no_server = [e for e in pre_http if e is not server_entry]
    for e in pre_http_no_server:
        text = _emit_entry(e, value_providers, guarded_tokens)
        if text:
            lines.append(text)

    handle_var = None
    if has_server:
        # The http_server-providing entry's `requires=` is a hard WireError
        # (checked above) -- its __auto_type capture line is therefore always
        # unguarded, and `handle_var` is always in scope for every downstream
        # `server=true` call.
        handle_var = "bb_app_http_handle"
        lines.append(f"    __auto_type {handle_var} = {server_entry.fn}();")
        avail = _emit_provides_avail(server_entry, guarded_tokens, success_expr=None)
        if avail:
            # rstrip the trailing "\n" avail already ends with -- "\n".join()
            # supplies the line break, so appending it unstripped plus the
            # blank-line separator below would double the blank line.
            lines.append(avail.rstrip("\n"))
        lines.append("")

    for e in regular:
        text = _emit_entry(e, value_providers, guarded_tokens, handle_var)
        if text:
            lines.append(text)

    lines += ["    return bb_app_first_err;", "}", ""]
    lines += [
        "bb_err_t bb_app_init(void)",
        "{",
        "    bb_err_t bb_app_first_err = bb_app_init_early();",
        "    bb_err_t bb_app_rest_err = bb_app_init_rest();",
        "    return (bb_app_first_err != BB_OK) ? bb_app_first_err : bb_app_rest_err;",
        "}",
        "",
    ]
    return "\n".join(lines)


def render_cmake_fragment(c_path_rel: str) -> str:
    return "\n".join([
        "# Generated by `bbtool codegen` -- do not hand-edit, do not commit (decision #725).",
        f'set(BB_WIRE_GENERATED_SOURCE "{c_path_rel}")',
        "",
    ])
