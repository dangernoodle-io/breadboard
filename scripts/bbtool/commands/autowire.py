"""autowire command — resolves an explicit component-name subset's transitive
closure via `boards.py`'s derivation plumbing
(`discover_components`/`derive_component`/`resolve_transitive`) and writes a
CMake fragment defining `BB_AUTOWIRE_REQUIRES`/`BB_AUTOWIRE_COMPONENTS` for a
consumer's build to control exactly which components ESP-IDF discovers and
links, instead of a hand-maintained REQUIRES list.

This is deliberately NOT `boards.build_graph()` — that function resolves a
board's component set from `[capability.*]`/`[board.*]` tables in a consumer's
bbtool.toml. This command takes an explicit component-name list directly (no
manifest), for one-off link-set experiments (e.g. flash-size comparisons)
where a full board manifest is overkill. `resolve_composition()` mirrors
`build_graph()`'s lazy-derive BFS loop exactly, minus the capability/board
resolution step.

Determinism: sorted output, no dict/set iteration-order leaks, no timestamps
(matches boards.py's conventions).
"""
from __future__ import annotations
import argparse
import os
import sys

from boards import discover_components, derive_component, resolve_transitive, ManifestError
from cmake_parse import ConditionalSetError

NAME = "autowire"
HELP = "resolve a component-name subset's transitive closure into a CMake REQUIRES/COMPONENTS fragment"

DEFAULT_PLATFORM = "espidf"
DEFAULT_OUT_REL = os.path.join("examples", "smoke", "main", "generated", "bb_autowire_components.cmake")


def add_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument(
        "--root",
        default=os.getcwd(),
        help="repository root (default: cwd)",
    )
    parser.add_argument(
        "--components",
        required=True,
        help="comma-separated component names",
    )
    parser.add_argument(
        "--platform",
        default=DEFAULT_PLATFORM,
        help=f"platform layer to resolve against (default: {DEFAULT_PLATFORM})",
    )
    parser.add_argument(
        "--out",
        default=None,
        help=f"output .cmake path (default: <root>/{DEFAULT_OUT_REL})",
    )


def register(api) -> None:
    api.add_command(NAME, sys.modules[__name__])


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


def render_cmake_fragment(components) -> str:
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
    """
    lines = [
        "# Generated by `bbtool autowire` -- do not hand-edit.",
        f"set(BB_AUTOWIRE_REQUIRES {' '.join(components)})",
        "# BB_AUTOWIRE_COMPONENTS: project-level ESP-IDF COMPONENTS allowlist.",
        "# Distinct lever from BB_AUTOWIRE_REQUIRES above: this one gates",
        "# component *discovery*, not just main's own REQUIRES list. Set it",
        "# BEFORE include($ENV{IDF_PATH}/tools/cmake/project.cmake).",
        f"set(BB_AUTOWIRE_COMPONENTS main {' '.join(components)})",
        "",
    ]
    return "\n".join(lines)


def run(args: argparse.Namespace) -> int:
    root = os.path.abspath(getattr(args, "root", None) or os.getcwd())

    names = [n.strip() for n in args.components.split(",") if n.strip()]
    if not names:
        print("bbtool autowire: error: --components must list at least one component", file=sys.stderr)
        return 1

    try:
        order = resolve_composition(root, names, args.platform)
    except (ManifestError, ConditionalSetError) as e:
        print(f"bbtool autowire: error: {e}", file=sys.stderr)
        return 1

    out_path = os.path.abspath(args.out) if args.out else os.path.join(root, DEFAULT_OUT_REL)
    out_dir = os.path.dirname(out_path)
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)
    with open(out_path, "w", encoding="utf-8") as f:
        f.write(render_cmake_fragment(order))

    print(f"bbtool autowire: wrote {out_path} ({len(order)} components)")
    for name in order:
        print(f"  {name}")
    return 0
