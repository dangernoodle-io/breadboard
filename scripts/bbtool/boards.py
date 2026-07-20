"""Capability/board manifest parsing + component build-graph derivation.

Reads `[capability.*]` and `[board.*]` tables out of the consumer's
bbtool.toml (see scripts/bbtool/README.md and the Phase-1 plan for the
schema) and DERIVES each active component's `{includes, sources, depends}`
from the component's own CMakeLists.txt (REQUIRES/PRIV_REQUIRES, via
cmake_parse) and the `components/<n>/` + `platform/{host,espidf,arduino}/<n>/`
directory convention — never hand-authored.

Determinism: every returned list is sorted; no dict/set iteration-order
leaks; no timestamps, no absolute paths.
"""
from __future__ import annotations
import os
from pathlib import Path
from typing import Dict, List, Optional, Set, Tuple

from cmake_parse import parse_requires, parse_hints
from discovery import PLATFORMS, build_index


class ManifestError(Exception):
    """Raised for capability/board/component manifest validation failures —
    always a hard error (never a silent skip): an unknown capability name, an
    unknown component name, or an unknown board."""


# ---------------------------------------------------------------------------
# Manifest parsing
# ---------------------------------------------------------------------------

def load_manifest(config: dict) -> Tuple[Dict[str, dict], Dict[str, dict]]:
    """Extract `[capability.*]` and `[board.*]` tables from the bbtool.toml
    config dict (as returned by core.load_config). Returns
    (capabilities, boards), each name -> its table body (dict), possibly
    empty if the config has no such section."""
    capabilities = dict(config.get("capability", {}) or {})
    boards = dict(config.get("board", {}) or {})
    return capabilities, boards


def required_capabilities(capabilities: Dict[str, dict]) -> List[str]:
    """Capabilities marked `required = true` — auto-applied to every board;
    sorted for determinism."""
    return sorted(name for name, cfg in capabilities.items() if cfg.get("required"))


def resolve_active_capabilities(
    board_name: str, boards: Dict[str, dict], capabilities: Dict[str, dict]
) -> List[str]:
    """A board's active capabilities = (every `required=true` capability) UNION
    (the board's own `capabilities` list). Sorted, de-duplicated. Raises
    ManifestError if the board is unknown or lists an undefined capability."""
    if board_name not in boards:
        raise ManifestError(
            f"unknown board '{board_name}'; known: {sorted(boards.keys())}"
        )
    board = boards[board_name]
    listed = list(board.get("capabilities", []) or [])
    for cap in listed:
        if cap not in capabilities:
            raise ManifestError(
                f"board '{board_name}' lists unknown capability '{cap}'; "
                f"known: {sorted(capabilities.keys())}"
            )
    active = set(required_capabilities(capabilities)) | set(listed)
    return sorted(active)


def resolve_component_names(
    board_name: str, boards: Dict[str, dict], capabilities: Dict[str, dict]
) -> List[str]:
    """Union each active capability's `components`/`add_components`, plus the
    board's own `add_components`, minus the board's `remove_components`.
    Sorted, de-duplicated."""
    active = resolve_active_capabilities(board_name, boards, capabilities)
    board = boards[board_name]

    names: Set[str] = set()
    for cap in active:
        cfg = capabilities[cap]
        names.update(cfg.get("components", []) or [])
        names.update(cfg.get("add_components", []) or [])

    names.update(board.get("add_components", []) or [])
    names.difference_update(board.get("remove_components", []) or [])
    return sorted(names)


# ---------------------------------------------------------------------------
# Component universe discovery
# ---------------------------------------------------------------------------

def discover_components(root: str) -> Set[str]:
    """The discovered component universe: every directory name under
    `components/` or `platform/{host,espidf,arduino}/`. This is the SSOT
    validation set — a manifest component name or a derived depends name that
    isn't in this set is a hard error (typo), never silently dropped.

    Thin wrapper over `discovery.build_index` (B1-979) — single-root, so
    never raises `discovery.CollisionError`."""
    return set(build_index([root]).names())


# ---------------------------------------------------------------------------
# Per-component derivation: {includes, sources, depends}
# ---------------------------------------------------------------------------

def _read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8") if path.is_file() else ""


def _glob_c_files(root: Path, dir_path: Path, recursive: bool) -> List[str]:
    """*.c files directly under dir_path (or recursively, honoring
    `recursive`), excluding any file under a `test/` subtree, as repo-root
    relative POSIX-style paths."""
    if not dir_path.is_dir():
        return []
    pattern = "**/*.c" if recursive else "*.c"
    out = []
    for c_file in dir_path.glob(pattern):
        rel = c_file.relative_to(root)
        if "test" in rel.parts[:-1]:
            continue
        out.append(str(rel).replace(os.sep, "/"))
    return out


def derive_component(root: str, name: str, platform: str) -> Dict[str, List[str]]:
    """Derive one component's `{includes, sources, depends}`:
      - depends = union(REQUIRES, PRIV_REQUIRES) from the component's own
        CMakeLists.txt (cmake_parse.parse_requires) — NOT yet filtered to the
        project universe; callers filter (build_graph does).
      - includes/sources = directory-convention walk over
        components/<name>/{include,src,.} and
        platform/<platform>/<name>/{include,.}, honoring the board's
        platform layer. `test/` subtrees are always excluded from source
        globs.
      - residual, non-derivable extras are read from
        `# bbtool-scaffold-hint: include=<path>` / `source=<path>` comments
        inside the component's own CMakeLists.txt.
    """
    root_p = Path(root)
    index = build_index([root])
    comp_dir = index.component_dir(name)
    plat_dir = index.platform_dir(name, platform)

    cmake_text = _read_text(comp_dir / "CMakeLists.txt") if comp_dir is not None else ""
    requires, priv_requires = parse_requires(cmake_text, component=name)
    depends: Set[str] = set(requires) | set(priv_requires)

    # A platform-only component (no components/<name>/ dir at all — e.g.
    # bb_event_routes_espidf) declares its own idf_component_register(...)
    # directly under platform/<platform>/<name>/CMakeLists.txt. Ignoring it
    # dropped that component's REQUIRES/PRIV_REQUIRES from the closure
    # entirely (B1-903): a component depended on ONLY via a platform-layer
    # PRIV_REQUIRES (e.g. bb_sse_writer) never got visited by
    # resolve_transitive and looked dead. Union both CMakeLists' depends —
    # a component may legitimately declare REQUIRES at either or both
    # layers.
    plat_cmake_text = _read_text(plat_dir / "CMakeLists.txt") if plat_dir is not None else ""
    plat_requires, plat_priv_requires = parse_requires(plat_cmake_text, component=name)
    depends |= set(plat_requires) | set(plat_priv_requires)
    depends = sorted(depends)

    includes: Set[str] = set()
    sources: Set[str] = set()

    if comp_dir is not None:
        comp_include = comp_dir / "include"
        if comp_include.is_dir():
            includes.add(f"components/{name}/include")

        comp_src = comp_dir / "src"
        if comp_src.is_dir():
            includes.add(f"components/{name}/src")
            sources.update(_glob_c_files(root_p, comp_src, recursive=False))
        else:
            # Flat component layout: top-level *.c files live directly under
            # components/<name>/ (e.g. bb_diag, bb_mdns) — the dir itself also
            # needs to be an include path (private headers colocated there).
            flat_sources = _glob_c_files(root_p, comp_dir, recursive=False)
            if flat_sources:
                includes.add(f"components/{name}")
                sources.update(flat_sources)

    if plat_dir is not None:
        plat_include = plat_dir / "include"
        if plat_include.is_dir():
            includes.add(f"platform/{platform}/{name}/include")
        else:
            includes.add(f"platform/{platform}/{name}")
        sources.update(_glob_c_files(root_p, plat_dir, recursive=False))

    hints = parse_hints(cmake_text)
    includes.update(hints.get("include", []))
    sources.update(hints.get("source", []))

    return {
        "includes": sorted(includes),
        "sources": sorted(sources),
        "depends": depends,
    }


# ---------------------------------------------------------------------------
# Transitive resolution (generalized native_scaffold.resolve_components)
# ---------------------------------------------------------------------------

def resolve_transitive(
    requested: List[str], graph: Dict[str, Dict[str, List[str]]], universe: Set[str]
) -> List[str]:
    """Walk `graph[name]["depends"]` from each requested component, returning
    the full transitive closure restricted to `universe`. Dependency-before-
    dependent order (stable); cycles tolerated via a visited set. Raises
    ManifestError naming the typo if a requested or depended-on name isn't in
    `universe`."""
    resolved: List[str] = []
    seen: Set[str] = set()

    def visit(comp_name: str) -> None:
        if comp_name in seen:
            return
        if comp_name not in universe:
            raise ManifestError(
                f"unknown component '{comp_name}'; not found under components/ "
                f"or platform/{{{','.join(PLATFORMS)}}}/"
            )
        seen.add(comp_name)
        entry = graph.get(comp_name)
        depends = entry.get("depends", []) if entry else []
        for dep in sorted(depends):
            if dep in universe:
                visit(dep)
        resolved.append(comp_name)

    for comp_name in sorted(requested):
        visit(comp_name)
    return resolved


# ---------------------------------------------------------------------------
# Full board resolution
# ---------------------------------------------------------------------------

def build_graph(root: str, board_name: str, config: dict) -> Dict[str, object]:
    """Resolve a board's full derived build graph from bbtool.toml.

    Returns:
      {
        "board": board_name,
        "platform": "host" | "espidf" | "arduino",
        "capabilities": [sorted active capability names],
        "order": [component names, dependency-before-dependent],
        "components": {name: {"includes": [...], "sources": [...], "depends": [...]}},
      }

    Raises ManifestError for any undefined board/capability/component
    reference (validation errors are always hard, never a silent skip).
    """
    capabilities, boards = load_manifest(config)
    if board_name not in boards:
        raise ManifestError(
            f"unknown board '{board_name}'; known: {sorted(boards.keys())}"
        )
    board = boards[board_name]
    platform = board.get("platform", "host")

    active_caps = resolve_active_capabilities(board_name, boards, capabilities)
    requested = resolve_component_names(board_name, boards, capabilities)

    universe = discover_components(root)
    for name in requested:
        if name not in universe:
            raise ManifestError(
                f"board '{board_name}' resolves unknown component '{name}'; "
                f"not found under components/ or platform/{{{','.join(PLATFORMS)}}}/"
            )

    # Derive every component in the universe reachable from `requested` —
    # derive lazily as we discover names via BFS so we never touch
    # components outside the closure.
    graph: Dict[str, Dict[str, List[str]]] = {}
    frontier = list(requested)
    visited_derive: Set[str] = set()
    while frontier:
        name = frontier.pop()
        if name in visited_derive:
            continue
        visited_derive.add(name)
        if name not in universe:
            # Referenced only as a CMake dep, not a real project component
            # (e.g. an ESP-IDF SDK component like esp_timer) — never derived,
            # filtered out by resolve_transitive's universe check below.
            continue
        entry = derive_component(root, name, platform)
        graph[name] = entry
        frontier.extend(d for d in entry["depends"] if d not in visited_derive)

    # Filter each component's depends to the project universe only — external
    # SDK/library deps (esp_timer, freertos, lwip, log, ...) aren't
    # scaffold-resolvable and are irrelevant to include/source wiring.
    for entry in graph.values():
        entry["depends"] = sorted(d for d in entry["depends"] if d in universe)

    order = resolve_transitive(requested, graph, universe)

    return {
        "board": board_name,
        "platform": platform,
        "capabilities": active_caps,
        "order": order,
        "components": graph,
    }
