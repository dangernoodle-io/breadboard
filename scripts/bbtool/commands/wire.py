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

DEFERRED (do not implement here): a PlatformIO pre-build hook wiring this in
automatically; a lint rule that validates marker hygiene.
"""
from __future__ import annotations
import glob
import os
from typing import List, Tuple

from wire_parse import InitEntry, parse_markers

DEFAULT_OUT_REL = os.path.join("main", "generated", "bb_app_init.c")

HTTP_SERVER_PROVIDES_KEY = "http_server"


class WireError(Exception):
    """Hard error for wire-invariant violations (e.g. a server=true entry
    with no http_server provider in the set, or more than one / a
    mis-tiered http_server provider)."""


# ---------------------------------------------------------------------------
# Marker collection
# ---------------------------------------------------------------------------

def _component_headers(root: str, name: str, platform: str) -> List[str]:
    """Public header files for one component, repo-root-relative POSIX paths,
    sorted for determinism: components/<name>/include/*.h, falling back to
    components/<name>/*.h (flat layout), plus platform/<platform>/<name>/
    (include/ or flat) — mirrors boards.derive_component's directory
    convention."""
    headers: List[str] = []

    comp_include = os.path.join(root, "components", name, "include")
    if os.path.isdir(comp_include):
        headers.extend(sorted(glob.glob(os.path.join(comp_include, "*.h"))))
    else:
        comp_dir = os.path.join(root, "components", name)
        if os.path.isdir(comp_dir):
            headers.extend(sorted(glob.glob(os.path.join(comp_dir, "*.h"))))

    plat_include = os.path.join(root, "platform", platform, name, "include")
    if os.path.isdir(plat_include):
        headers.extend(sorted(glob.glob(os.path.join(plat_include, "*.h"))))
    else:
        plat_dir = os.path.join(root, "platform", platform, name)
        if os.path.isdir(plat_dir):
            headers.extend(sorted(glob.glob(os.path.join(plat_dir, "*.h"))))

    return [os.path.relpath(h, root).replace(os.sep, "/") for h in headers]


def collect_entries(root: str, components: List[str], platform: str) -> List[InitEntry]:
    """Grep every resolved component's public header(s), in composition
    order (dependency-before-dependent, as returned by resolve_composition),
    then header-path order, then in-file line order — this fixed walk order
    is the "parse order" wire_graph.topo_sort tie-breaks on."""
    entries: List[InitEntry] = []
    for name in components:
        for rel_header in _component_headers(root, name, platform):
            abs_header = os.path.join(root, rel_header)
            with open(abs_header, encoding="utf-8") as f:
                text = f.read()
            entries.extend(parse_markers(text, src_file=rel_header))
    return entries


# ---------------------------------------------------------------------------
# Rendering
# ---------------------------------------------------------------------------

def _headers_for(entries: List[InitEntry]) -> List[str]:
    return sorted({e.src_file for e in entries})


def _http_server_providers(entries: List[InitEntry]) -> List[InitEntry]:
    return [e for e in entries if HTTP_SERVER_PROVIDES_KEY in e.provides]


def _emit_call(entry: InitEntry, handle_var: str = None) -> str:
    arg = handle_var if (entry.server and handle_var) else ""
    return (
        f"    bb_app_rc = {entry.fn}({arg});\n"
        f"    if (bb_app_rc != BB_OK && bb_app_first_err == BB_OK) {{ "
        f"bb_app_first_err = bb_app_rc; }}\n"
    )


def render_source(ordered: List[InitEntry]) -> str:
    """Render bb_app_init.c from a fully tier-ordered entry list (as returned
    by wire_graph.topo_sort).

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
        resolved set at all.
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

    headers = _headers_for(ordered)
    include_lines = "\n".join(f'#include "{h.split("/")[-1]}"' for h in headers)

    lines: List[str] = [
        "/* Generated by `bbtool codegen` -- DO NOT EDIT, DO NOT COMMIT.",
        " * Regenerated every build (decision #725); source of truth is the",
        " * `// bbtool:init` markers this file was grepped from (decision #735).",
        " */",
        include_lines,
        "",
        "bb_err_t bb_app_init_early(void)",
        "{",
        "    bb_err_t bb_app_first_err = BB_OK;",
        "    bb_err_t bb_app_rc;",
        "",
    ]
    for e in early:
        lines.append(_emit_call(e))
    lines += ["    return bb_app_first_err;", "}", "", "bb_err_t bb_app_init_rest(void)", "{"]
    lines += ["    bb_err_t bb_app_first_err = BB_OK;", "    bb_err_t bb_app_rc;", ""]

    pre_http_no_server = [e for e in pre_http if e is not server_entry]
    for e in pre_http_no_server:
        lines.append(_emit_call(e))

    handle_var = None
    if has_server:
        handle_var = "bb_app_http_handle"
        lines.append(f"    __auto_type {handle_var} = {server_entry.fn}();")
        lines.append("")

    for e in regular:
        lines.append(_emit_call(e, handle_var))

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
