"""Composition resolver — shared by `bbtool codegen` (the sole surviving
composition path besides handwire).

Resolves an explicit component-name subset's transitive closure via
`boards.py`'s derivation plumbing (`discover_components`/`derive_component`/
`resolve_transitive`) and renders a CMake fragment defining
`BB_AUTOWIRE_REQUIRES`/`BB_AUTOWIRE_COMPONENTS` for a consumer's build to
control exactly which components ESP-IDF discovers and links, instead of a
hand-maintained REQUIRES list.

This is deliberately NOT `boards.build_graph()` — that function resolves a
board's component set from `[capability.*]`/`[board.*]` tables in a consumer's
bbtool.toml. `resolve_composition` takes an explicit component-name list
directly (no manifest). `resolve_composition()` mirrors `build_graph()`'s
lazy-derive BFS loop exactly, minus the capability/board resolution step.

Determinism: sorted output, no dict/set iteration-order leaks, no timestamps
(matches boards.py's conventions).

Relocated out of the (now-deleted) `bbtool autowire` CLI command — this
module is the surviving resolver `bbtool codegen` needs; it has no CLI
surface of its own. The `BB_AUTOWIRE_*` CMake variable names and the
`bb_autowire_components.cmake` output filename are kept verbatim (codegen's
established output contract); only the dead standalone `autowire` command
was removed.
"""
from __future__ import annotations
import os
import re
from pathlib import Path

from boards import discover_components, derive_component, resolve_transitive
from discovery import build_index, normalize_roots

DEFAULT_PLATFORM = "espidf"
DEFAULT_OUT_REL = os.path.join("examples", "smoke", "main", "generated", "bb_autowire_components.cmake")

# B1-985: the format registry itself (bb_serialize) deliberately carries no
# privileged default backend (decided in B1-981) -- a component that walks
# through it (e.g. bb_cache_serialize_get()/bb_data's render path) compiles
# fine with zero bb_serialize_* backends composed alongside it, then fails
# every format lookup at RUNTIME with no build-time signal. See
# check_format_registry_backends below.
FORMAT_REGISTRY_COMPONENT = "bb_serialize"

# Real call-site detection (reworked per B1-985 author decision): a component
# merely REQUIRES-ing bb_serialize (e.g. for the bb_serialize_desc_t TYPE --
# bb_tcp_client, bb_mqtt_client, bb_http_client, bb_meminfo, bb_system) is
# NOT a format-registry consumer. Only a component whose C sources actually
# call the registry's lookup/dispatch entry points is a consumer -- matches
# how bb_data.c and platform/espidf/bb_cache_serialize's render path invoke
# it (bb_serialize_format_get_render()/bb_serialize_format_render(), per
# components/bb_serialize/include/bb_serialize_format.h). The pattern set
# covers BOTH sides of the registry lookup: render (bb_serialize_format_
# render()/bb_serialize_format_get_render()) and parse (bb_serialize_format_
# get_parse()) -- a parse-only consumer fails the same silent way at
# runtime, even though no in-tree component calls it yet. Likewise, a
# `bb_serialize_*`-prefixed component is only a BACKEND if it actually calls
# bb_serialize_format_register() -- a future non-registering bb_serialize_*
# helper must not count.
_CONSUMER_CALL_PATTERNS = (
    re.compile(r"\bbb_serialize_format_render\s*\("),
    re.compile(r"\bbb_serialize_format_get_render\s*\("),
    re.compile(r"\bbb_serialize_format_get_parse\s*\("),
)
_BACKEND_CALL_PATTERN = re.compile(r"\bbb_serialize_format_register\s*\(")

_BLOCK_COMMENT_RE = re.compile(r"/\*.*?\*/", re.DOTALL)
_LINE_COMMENT_RE = re.compile(r"//[^\n]*")


def _strip_c_comments(text: str) -> str:
    """Strip `//` line comments and `/* ... */` block comments before regex
    scanning -- a call-site pattern MENTIONED in a comment (e.g. a docstring
    referencing `bb_serialize_format_register()` alongside an actual
    `bb_serialize_format_get_render()` call in the same file, as
    platform/espidf/bb_cache_serialize's render path does) must not
    misclassify the component. Does not special-case comment markers inside
    string literals -- these function names never appear in string literals
    in this codebase."""
    text = _BLOCK_COMMENT_RE.sub(" ", text)
    return _LINE_COMMENT_RE.sub(" ", text)


def _read_component_sources(root: str, sources) -> str:
    """Concatenated, comment-stripped text of a component's already-derived
    `sources` list (repo-root-relative paths, as returned by
    `derive_component`/the composition graph) -- read once per component, no
    re-derivation."""
    root_p = Path(root)
    chunks = []
    for rel in sources:
        path = root_p / rel
        if path.is_file():
            chunks.append(_strip_c_comments(path.read_text(encoding="utf-8")))
    return "\n".join(chunks)


def check_format_registry_backends(roots, components, graph):
    """Composition-time mis-wiring check (B1-985, precedent B1-981): given a
    resolved composition (`components`, as returned by `resolve_composition`)
    and its already-built `{name: {includes,sources,depends}}` component
    graph (as built internally by `resolve_composition`/
    `resolve_composition_with_graph` -- never re-derived here), return a
    warning string when the set contains a *format-registry consumer* --
    a component whose C sources actually call the registry's render
    lookup/dispatch entry points -- while composing ZERO components whose C
    sources call `bb_serialize_format_register()`. Returns `None` when
    there's nothing to warn about (registry not composed at all, at least one
    real backend present, or no real consumer reachable).

    `roots` is a single root (str, back-compat) or an ordered list of roots
    (B1-1084) -- each component's sources are read from ITS OWN owning root
    (`discovery.build_index(roots).entry(name).root`), never blindly from
    `roots[0]`, so a format-registry consumer/backend discovered under a
    non-primary root is still read correctly.

    Non-fatal by design (WARN, not a hard error) -- a composition missing a
    backend is a legitimate host/scaffold subset in some cases; the point is
    to surface the silent BB_ERR_UNSUPPORTED trap at build time instead of
    leaving it purely a runtime surprise."""
    names = set(components)
    if FORMAT_REGISTRY_COMPONENT not in names:
        return None

    roots_list = normalize_roots(roots)
    index = build_index(roots_list)
    primary_root = roots_list[0] if roots_list else ""

    backends = []
    consumers = []
    for name in sorted(names):
        if name == FORMAT_REGISTRY_COMPONENT:
            continue
        entry = graph.get(name) or {}
        owning_entry = index.entry(name)
        entry_root = owning_entry.root if owning_entry is not None else primary_root
        text = _read_component_sources(entry_root, entry.get("sources", []))
        # Backend and consumer are detected INDEPENDENTLY -- a component can
        # legitimately be both (e.g. a format backend that also looks up
        # another format's render fn). A backend match must never suppress
        # consumer detection, and vice versa.
        if _BACKEND_CALL_PATTERN.search(text):
            backends.append(name)
        if any(pattern.search(text) for pattern in _CONSUMER_CALL_PATTERNS):
            consumers.append(name)

    if backends or not consumers:
        return None

    return (
        "bbtool: warning: format-registry consumer(s) ["
        + ", ".join(consumers)
        + "] composed with zero bb_serialize_* backends -- format lookups "
        "(e.g. bb_cache_serialize_get()) will return BB_ERR_UNSUPPORTED for "
        "every format at runtime; compose a bb_serialize_* backend (e.g. "
        "bb_serialize_json) alongside it"
    )


def _build_composition_graph(roots, names, platform: str = DEFAULT_PLATFORM):
    """Shared BFS derive loop behind `resolve_composition`/
    `resolve_composition_with_graph` -- derives each named component's
    {includes,sources,depends} lazily over `depends`, exactly mirroring
    `boards.build_graph()`'s derive loop, but for an explicit requested set
    with no capability/board manifest resolution step. `roots` is a single
    root (str, back-compat) or an ordered list of roots (B1-1084) —
    `discover_components`/`derive_component` resolve each name against its
    OWN owning root (see `boards.derive_component`'s docstring). Returns
    `(graph, universe)`; callers combine with `resolve_transitive` for the
    ordered closure."""
    universe = discover_components(roots)

    graph = {}
    frontier = list(names)
    visited = set()
    while frontier:
        name = frontier.pop()
        if name in visited:
            continue
        visited.add(name)
        if name not in universe:
            # Referenced only as a CMake dep, not a real project component
            # (e.g. an ESP-IDF SDK component like esp_timer) — never derived;
            # resolve_transitive's universe check filters it out below.
            continue
        entry = derive_component(roots, name, platform)
        graph[name] = entry
        frontier.extend(d for d in entry["depends"] if d not in visited)

    for entry in graph.values():
        entry["depends"] = sorted(d for d in entry["depends"] if d in universe)

    return graph, universe


def resolve_composition(roots, names, platform: str = DEFAULT_PLATFORM):
    """Compute the transitive closure over `names` — Raises `ManifestError`
    for any name (requested or transitively depended-on) not found under
    `components/` or `platform/{host,espidf,arduino}/` — `resolve_transitive`
    performs this check for every name, requested or transitive, so no
    separate pre-check is needed here. `roots` is a single root (str,
    back-compat) or an ordered list of roots (B1-1084)."""
    graph, universe = _build_composition_graph(roots, names, platform)
    return resolve_transitive(names, graph, universe)


def resolve_composition_with_graph(roots, names, platform: str = DEFAULT_PLATFORM):
    """Same computation as `resolve_composition`, but also returns the
    already-built `{name: {includes,sources,depends}}` component graph so a
    caller needing per-component source lists (e.g. B1-985's
    `check_format_registry_backends`) reuses it instead of re-deriving each
    component from scratch. `roots` is a single root (str, back-compat) or an
    ordered list of roots (B1-1084). Returns `(components, graph)`."""
    graph, universe = _build_composition_graph(roots, names, platform)
    components = resolve_transitive(names, graph, universe)
    return components, graph


def render_cmake_fragment(components, board=None) -> str:
    """CMake fragment defining two variables from the same resolved
    composition:

    - `BB_AUTOWIRE_REQUIRES` — space-separated list, matching
      examples/smoke/main/CMakeLists.txt's existing `SMOKE_REQUIRES`
      formatting. Consumed by a component's own `idf_component_register(...
      REQUIRES ${BB_AUTOWIRE_REQUIRES})` — this only controls what `main`
      (or whichever component includes this fragment) declares itself to
      require; it does NOT gate which components ESP-IDF *discovers* under
      `EXTRA_COMPONENT_DIRS` — REQUIRES alone can leave excluded components
      linked anyway if something else in the build still reaches them.
    - `BB_AUTOWIRE_COMPONENTS` — `BB_AUTOWIRE_REQUIRES` prefixed with
      `main`, formatted for the project-level ESP-IDF `COMPONENTS` variable
      (`set(COMPONENTS ${BB_AUTOWIRE_COMPONENTS})`, set BEFORE
      `include($ENV{IDF_PATH}/tools/cmake/project.cmake)`). Per the ESP-IDF
      build-system docs, `COMPONENTS` actually restricts component
      *discovery*: only the named
      components (here, `main`) plus their transitive REQUIRES/PRIV_REQUIRES
      (recursively, resolved by ESP-IDF itself from each discovered
      component's own CMakeLists.txt) are configured and built at all —
      everything else under EXTRA_COMPONENT_DIRS is never even discovered.

    `board`, when given (B1-747 manifest-driven `bbtool codegen --board`),
    also defines `BB_AUTOWIRE_BOARD` — the board id this REQUIRES closure was
    resolved for. Since the closure is now board-specific but the generated
    dir is gitignored, a consumer's CMakeLists.txt should FATAL_ERROR if this
    doesn't match the board it's actually building (stale-fragment guard —
    a leftover fragment from a different board's `make smoke-gen-*` run
    would otherwise silently link the wrong closure).
    """
    lines = ["# Generated by `bbtool codegen` -- do not hand-edit."]
    if board is not None:
        lines.append(f'set(BB_AUTOWIRE_BOARD "{board}")')
    lines.append(f"set(BB_AUTOWIRE_REQUIRES {' '.join(components)})")
    lines.append("# BB_AUTOWIRE_COMPONENTS: project-level ESP-IDF COMPONENTS allowlist.")
    lines.append("# Distinct lever from BB_AUTOWIRE_REQUIRES above: this one gates")
    lines.append("# component *discovery*, not just main's own REQUIRES list. Set it")
    lines.append("# BEFORE include($ENV{IDF_PATH}/tools/cmake/project.cmake).")
    lines.append(f"set(BB_AUTOWIRE_COMPONENTS main {' '.join(components)})")
    lines.append("")
    return "\n".join(lines)
