"""codegen command: the sole composition-generation CLI (the dead `bbtool
autowire`/`bbtool wire` spike commands have been removed; their surviving
logic lives on as library code this command calls into).

Resolves the composition's transitive closure exactly ONCE via
`composition.resolve_composition()`, then emits BOTH artifacts from that
single resolution:

  1. The COMPONENTS link-set fragment (`bb_autowire_components.cmake`).
  2. `bb_app_init.c` (+ sibling `.cmake`) wired from `// bbtool:init`
     markers.

Neither underlying algorithm is reimplemented here: the CMake-fragment
renderer lives in `composition`, and the topo-sort/marker-parsing/source-
renderer live in `wire_parse` / `wire_graph` / `commands.wire` — all simply
called into.

Exactly one of `--components` (explicit override; the escape hatch the
synthetic-fixture tests rely on) or `--board` (B1-747: resolves the
requested set from the `[capability.*]`/`[board.*]` manifest in bbtool.toml
via `boards.resolve_component_names` — see boards.py) is required.

`--board` board-parameterizes the REQUIRES/components-fragment ONLY. Wire
generation (`bb_app_init.c`) deliberately keeps resolving a fixed,
board-invariant baseline (today's pre-manifest behavior) via `--wire-board`
(defaults to `--board`'s value when omitted) — see the `run()` docstring
below for why.
"""
from __future__ import annotations
import argparse
import inspect
import os
import sys

from boards import ManifestError, load_manifest, resolve_component_names
from cmake_parse import ConditionalSetError
from composition import (
    DEFAULT_OUT_REL as COMPOSITION_DEFAULT_OUT_REL,
    DEFAULT_PLATFORM,
    check_format_registry_backends,
    render_cmake_fragment as render_components_fragment,
    resolve_composition,
    resolve_composition_with_graph,
)
from commands.wire import (
    DEFAULT_OUT_REL as WIRE_DEFAULT_OUT_REL,
    WireError,
    collect_entries,
    collect_provides_entries,
    render_cmake_fragment as render_wire_cmake_fragment,
    render_source,
)
from core import load_config
from discovery import CollisionError, normalize_roots
from wire_graph import CycleError, MissingProviderError, topo_sort
from wire_parse import ParseError

# Capture this file's real path at module load time (handles SCons exec()) —
# same pattern commands/version.py's pio_main uses to derive the breadboard
# repo root regardless of cwd/exec context.
_THIS_FILE = os.path.abspath(inspect.currentframe().f_code.co_filename)
_THIS_DIR = os.path.dirname(os.path.realpath(_THIS_FILE))

NAME = "codegen"
HELP = "codegen: resolve the composition once, emit both the link-set fragment and bb_app_init.c"


def add_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--root", default=os.getcwd(), help="repository root (default: cwd)")
    parser.add_argument(
        "--extra-root", action="append", default=[], dest="extra_root",
        help="additional discovery root (B1-1084; repeatable) — resolved after --root "
             "and after any [discovery].extra_roots in bbtool.toml, in the order given. "
             "A component name discovered under more than one root is a hard error "
             "(discovery.CollisionError) -- no shadow/override.",
    )
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument(
        "--components",
        help="comma-separated component names (explicit override)",
    )
    group.add_argument(
        "--board",
        help="board id from [board.<id>] in bbtool.toml -- resolves the "
             "REQUIRES/components-fragment set via the capability/board manifest",
    )
    parser.add_argument(
        "--wire-board", default=None,
        help="board id resolving the WIRE (bb_app_init.c) baseline when --board is "
             "given; defaults to --board's value. Wire generation is deliberately "
             "board-invariant -- pass a fixed baseline board id here to keep it "
             "pinned regardless of which board's REQUIRES set --board resolves.",
    )
    parser.add_argument(
        "--platform", default=DEFAULT_PLATFORM,
        help=f"platform layer to resolve against (default: {DEFAULT_PLATFORM})",
    )
    parser.add_argument(
        "--components-out", default=None,
        help=f"output link-set .cmake path (default: <root>/{COMPOSITION_DEFAULT_OUT_REL})",
    )
    parser.add_argument(
        "--wire-out", default=None,
        help=f"output bb_app_init.c path (default: <root>/{WIRE_DEFAULT_OUT_REL}); a "
             "sibling '.cmake' is written alongside it",
    )


def register(api) -> None:
    api.add_command(NAME, sys.modules[__name__])


def _toml_extra_roots(config: dict, config_dir: str) -> list:
    """B1-1084 Fork 1: `[discovery] extra_roots = [...]` in the consumer's
    bbtool.toml, resolved relative to the toml file's own dir — mirrors the
    existing `[plugins].paths`/`core.load_plugins` `config_dir` convention
    (cli.py: `os.path.dirname(pre_args.config) if pre_args.config else root`).
    An already-absolute entry is used verbatim."""
    discovery_cfg = (config or {}).get("discovery", {}) or {}
    return [
        r if os.path.isabs(r) else os.path.join(config_dir, r)
        for r in discovery_cfg.get("extra_roots", []) or []
    ]


def _resolve_roots(root: str, config: dict, config_dir: str, cli_extra_roots) -> list:
    """B1-1084 Fork 1: the full ordered discovery root list --
    `[--root] + (toml [discovery].extra_roots, file order) +
    (--extra-root flags, CLI order)`. No shadow/priority: a name discovered
    under more than one of these is a hard `discovery.CollisionError`
    (B1-979 model) -- see `discovery.build_index`."""
    toml_roots = _toml_extra_roots(config, config_dir)
    cli_roots = [
        r if os.path.isabs(r) else os.path.abspath(r)
        for r in (cli_extra_roots or [])
    ]
    return normalize_roots([root] + toml_roots + cli_roots)


def run(args: argparse.Namespace) -> int:
    """Resolve REQUIRES (board-parameterized when `--board` is given, B1-747)
    and WIRE (deliberately board-invariant) as two SEPARATE resolutions when
    `--board`/`--wire-board` diverge, so board-parameterizing the REQUIRES
    fragment can never change `bb_app_init.c`'s generated output: a
    `// bbtool:init`-marked component newly reachable only via a board's
    `add_components` (e.g. bb_display_info on a display board) must not
    start getting wired just because its headers are now visible. Both
    resolutions collapse to the SAME call when `--components` is used (the
    legacy/explicit-override path, unchanged) or when `--wire-board` isn't
    given and equals `--board` (e.g. examples/floor, which has no per-board
    variance at all)."""
    root = os.path.abspath(getattr(args, "root", None) or os.getcwd())

    components_arg = getattr(args, "components", None)
    board_arg = getattr(args, "board", None)
    if bool(components_arg) == bool(board_arg):
        print(
            "bbtool codegen: error: exactly one of --components or --board is required",
            file=sys.stderr,
        )
        return 1

    config = getattr(args, "_config_dict", None)
    if config is None:
        config = load_config(getattr(args, "config", None), root)
    config_path = getattr(args, "config", None)
    config_dir = os.path.dirname(config_path) if config_path else root

    # B1-1084: full ordered discovery root list -- [--root] + toml
    # [discovery].extra_roots + --extra-root flags. Single-root callers (no
    # [discovery] table, no --extra-root) get roots == [root], byte-identical
    # to pre-B1-1084 discovery.
    roots = _resolve_roots(root, config, config_dir, getattr(args, "extra_root", None))

    board_guard = None
    try:
        if board_arg:
            capabilities, boards_map = load_manifest(config)
            names = resolve_component_names(board_arg, boards_map, capabilities)
            wire_board_arg = getattr(args, "wire_board", None) or board_arg
            wire_names = (
                names if wire_board_arg == board_arg
                else resolve_component_names(wire_board_arg, boards_map, capabilities)
            )
            board_guard = board_arg
        else:
            names = [n.strip() for n in (components_arg or "").split(",") if n.strip()]
            wire_names = names

        if not names:
            if board_arg:
                print(
                    f"bbtool codegen: error: board '{board_arg}' resolved to no components; "
                    "check its [board.*]/[capability.*] tables in bbtool.toml",
                    file=sys.stderr,
                )
            else:
                print("bbtool codegen: error: --components must list at least one component", file=sys.stderr)
            return 1

        # REQUIRES/components-fragment resolution -- board-parameterized.
        # Also returns the already-built component graph so the B1-985
        # backend-registry check below reuses it (no re-derivation).
        components, graph = resolve_composition_with_graph(roots, names, args.platform)

        # B1-985: warn (non-fatal) when the resolved composition pulls a
        # format-registry consumer (bb_cache_serialize, bb_data) but zero
        # bb_serialize_* backends -- see composition.py's docstring.
        warning = check_format_registry_backends(roots, components, graph)
        if warning:
            print(warning, file=sys.stderr)

        # WIRE resolution -- deliberately board-invariant (see docstring
        # above); reuse `components` when the two requested sets are
        # identical rather than re-deriving, so the invariant path is
        # trivially provably the same computation as before B1-747.
        wire_components = components if wire_names == names else resolve_composition(roots, wire_names, args.platform)

        entries = collect_entries(roots, wire_components, args.platform)
        provides_entries = collect_provides_entries(roots, wire_components, args.platform)
        ordered = topo_sort(entries)
        source = render_source(ordered, provides_entries)
    except (ManifestError, ConditionalSetError, ParseError, CycleError, MissingProviderError,
            WireError, CollisionError) as e:
        print(f"bbtool codegen: error: {e}", file=sys.stderr)
        return 1

    components_out = args.components_out or os.path.join(root, COMPOSITION_DEFAULT_OUT_REL)
    os.makedirs(os.path.dirname(components_out), exist_ok=True)
    with open(components_out, "w", encoding="utf-8") as f:
        f.write(render_components_fragment(components, board=board_guard))

    wire_out = args.wire_out or os.path.join(root, WIRE_DEFAULT_OUT_REL)
    wire_cmake_out = os.path.splitext(wire_out)[0] + ".cmake"
    os.makedirs(os.path.dirname(wire_out), exist_ok=True)
    with open(wire_out, "w", encoding="utf-8") as f:
        f.write(source)
    with open(wire_cmake_out, "w", encoding="utf-8") as f:
        f.write(render_wire_cmake_fragment(os.path.relpath(wire_out, root).replace(os.sep, "/")))

    print(f"bbtool codegen: wrote {components_out} ({len(components)} components)")
    for name in components:
        print(f"  {name}")
    print(f"bbtool codegen: wrote {wire_out} ({len(ordered)} init entries)")
    print(f"bbtool codegen: wrote {wire_cmake_out}")
    for e in ordered:
        print(f"  [{e.tier}] {e.fn} ({e.src_file}:{e.src_line})")
    return 0


# ---------------------------------------------------------------------------
# PlatformIO / SCons pre-script entry (B1-1084 Fork 3)
# ---------------------------------------------------------------------------

def pio_main(env, root: str, board: str, config: dict) -> None:
    """SCons path, mirroring `commands.version.pio_main` /
    `commands.scaffold.pio_main`'s signature shape. NOT wired into
    `scripts/bbtool_pio.py` yet -- that's B1-1085's job; this entry point
    exists now so B1-1085 is pure wiring, not a re-litigated signature.

    `root` is the consumer's project dir (PROJECT_DIR). breadboard's own
    component root is computed the same way `version.pio_main` derives its
    `bb_dir` (this file's location -> ../../../ = breadboard repo root, via
    the module-level `_THIS_DIR` capture) and is always the PRIMARY discovery
    root; `[discovery].extra_roots` in `config` (resolved relative to `root`
    -- a consumer's own bbtool.toml lives at its own project root, mirroring
    `_toml_extra_roots`'s config_dir convention for the CLI path) are
    appended after it. `board` resolves the composition from
    `[capability.*]`/`[board.*]` in `config`, the same manifest
    `boards.build_graph`/`scaffold.pio_main` read -- `pio_main` has no
    `--components` escape hatch (that's a CLI-only override); a consumer
    always drives this via its board manifest.

    `env.Exit(1)` on any resolution error, mirroring `scaffold.pio_main`'s
    error convention (never a raised exception across the SCons boundary)."""
    bb_root = os.path.abspath(os.path.join(_THIS_DIR, "..", "..", ".."))
    roots = normalize_roots([bb_root] + _toml_extra_roots(config, root))

    try:
        capabilities, boards_map = load_manifest(config)
        names = resolve_component_names(board, boards_map, capabilities)
        if not names:
            raise ManifestError(
                f"board '{board}' resolved to no components; check its "
                "[board.*]/[capability.*] tables in bbtool.toml"
            )
        components, graph = resolve_composition_with_graph(roots, names, DEFAULT_PLATFORM)
        warning = check_format_registry_backends(roots, components, graph)
        if warning:
            print(warning)
        entries = collect_entries(roots, components, DEFAULT_PLATFORM)
        provides_entries = collect_provides_entries(roots, components, DEFAULT_PLATFORM)
        ordered = topo_sort(entries)
        source = render_source(ordered, provides_entries)
    except (ManifestError, ConditionalSetError, ParseError, CycleError, MissingProviderError,
            WireError, CollisionError) as e:
        print(f"bbtool codegen: error: {e}")
        env.Exit(1)
        return

    components_out = os.path.join(root, COMPOSITION_DEFAULT_OUT_REL)
    os.makedirs(os.path.dirname(components_out), exist_ok=True)
    with open(components_out, "w", encoding="utf-8") as f:
        f.write(render_components_fragment(components, board=board))

    wire_out = os.path.join(root, WIRE_DEFAULT_OUT_REL)
    wire_cmake_out = os.path.splitext(wire_out)[0] + ".cmake"
    os.makedirs(os.path.dirname(wire_out), exist_ok=True)
    with open(wire_out, "w", encoding="utf-8") as f:
        f.write(source)
    with open(wire_cmake_out, "w", encoding="utf-8") as f:
        f.write(render_wire_cmake_fragment(os.path.relpath(wire_out, root).replace(os.sep, "/")))

    print(f"bb_codegen: wrote {components_out} ({len(components)} components)")
    print(f"bb_codegen: wrote {wire_out} ({len(ordered)} init entries)")
    print(f"bb_codegen: wrote {wire_cmake_out}")
