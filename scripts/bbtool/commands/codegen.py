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
import os
import sys

from boards import ManifestError, load_manifest, resolve_component_names
from cmake_parse import ConditionalSetError
from composition import (
    DEFAULT_OUT_REL as COMPOSITION_DEFAULT_OUT_REL,
    DEFAULT_PLATFORM,
    render_cmake_fragment as render_components_fragment,
    resolve_composition,
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
from wire_graph import CycleError, MissingProviderError, topo_sort
from wire_parse import ParseError

NAME = "codegen"
HELP = "codegen: resolve the composition once, emit both the link-set fragment and bb_app_init.c"


def add_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--root", default=os.getcwd(), help="repository root (default: cwd)")
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

    board_guard = None
    try:
        if board_arg:
            config = getattr(args, "_config_dict", None)
            if config is None:
                config = load_config(None, root)
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
        components = resolve_composition(root, names, args.platform)

        # WIRE resolution -- deliberately board-invariant (see docstring
        # above); reuse `components` when the two requested sets are
        # identical rather than re-deriving, so the invariant path is
        # trivially provably the same computation as before B1-747.
        wire_components = components if wire_names == names else resolve_composition(root, wire_names, args.platform)

        entries = collect_entries(root, wire_components, args.platform)
        provides_entries = collect_provides_entries(root, wire_components, args.platform)
        ordered = topo_sort(entries)
        source = render_source(ordered, provides_entries)
    except (ManifestError, ConditionalSetError, ParseError, CycleError, MissingProviderError, WireError) as e:
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
