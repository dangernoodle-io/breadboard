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

from boards import discover_components, derive_component, resolve_transitive

DEFAULT_PLATFORM = "espidf"
DEFAULT_OUT_REL = os.path.join("examples", "smoke", "main", "generated", "bb_autowire_components.cmake")


def resolve_composition(root: str, names, platform: str = DEFAULT_PLATFORM):
    """Compute the transitive closure over `names` by deriving each named
    component's {includes,sources,depends} lazily (BFS over `depends`),
    exactly mirroring `boards.build_graph()`'s derive loop — but `names` is
    the requested set directly, with no capability/board manifest resolution
    step. Raises `ManifestError` for any name (requested or transitively
    depended-on) not found under `components/` or `platform/{host,espidf,
    arduino}/` — `resolve_transitive` performs this check for every name,
    requested or transitive, so no separate pre-check is needed here."""
    universe = discover_components(root)

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
        entry = derive_component(root, name, platform)
        graph[name] = entry
        frontier.extend(d for d in entry["depends"] if d not in visited)

    for entry in graph.values():
        entry["depends"] = sorted(d for d in entry["depends"] if d in universe)

    return resolve_transitive(names, graph, universe)


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
