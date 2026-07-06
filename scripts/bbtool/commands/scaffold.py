"""scaffold command — derive and print (or wire into a PlatformIO build) a
board's component build graph from the `[capability.*]`/`[board.*]` manifest
in bbtool.toml.

Subcommands:
    bbtool scaffold gen [--board <id>]   Print the resolved graph
                                         (includes/sources/depends + BB_CAP_*)
                                         for inspection.

Also exposes `pio_main(env)` for the PlatformIO pre-hook (wired via
scripts/bbtool_pio.py, exactly like `version`): reads the manifest for the
board being built, resolves the derived graph, and mutates env BUILD_FLAGS
(-I per include, -DBB_CAP_<NAME>=1) + build_src_filter (+<src>) — replacing
native_scaffold.py's hand-maintained COMPONENT_MAP.

No graph artifact is committed — every build (or `gen` invocation) re-reads
the manifest and re-derives live, so a manifest-only change is always
reflected (Phase 1 design: "generation model — PIO pre-hook, not
commit-and-remember").

Determinism: sorted output, no dict/set iteration-order leaks, no timestamps.
"""
from __future__ import annotations
import argparse
import os
import sys

from boards import build_graph, ManifestError

NAME = "scaffold"
HELP = "Derive and print a board's component build graph from the capability/board manifest"


# ---------------------------------------------------------------------------
# bbtool command interface
# ---------------------------------------------------------------------------

def add_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument(
        "--root",
        default=os.getcwd(),
        help="repository root (default: from top-level --root or cwd)",
    )
    parser.add_argument(
        "action",
        choices=["gen"],
        help="scaffold subcommand: 'gen' prints the resolved build graph",
    )
    parser.add_argument(
        "--board",
        default=None,
        help="board id from [board.<id>] in bbtool.toml (required for 'gen')",
    )


def register(api) -> None:
    api.add_command(NAME, sys.modules[__name__])


def _format_graph(graph: dict) -> str:
    lines = [
        f"board: {graph['board']}",
        f"platform: {graph['platform']}",
        f"capabilities: {', '.join(graph['capabilities'])}",
        "",
    ]
    for name in graph["order"]:
        entry = graph["components"][name]
        lines.append(f"[{name}]")
        lines.append(f"  depends:  {', '.join(entry['depends'])}")
        for inc in entry["includes"]:
            lines.append(f"  include:  {inc}")
        for src in entry["sources"]:
            lines.append(f"  source:   {src}")
    lines.append("")
    lines.append("build flags:")
    for cap in graph["capabilities"]:
        lines.append(f"  -DBB_CAP_{cap.upper()}=1")
    return "\n".join(lines) + "\n"


def run(args: argparse.Namespace) -> int:
    root = getattr(args, "root", None) or getattr(args, "_root_abs", None) or os.getcwd()
    root = os.path.abspath(root)
    config = getattr(args, "_config_dict", None) or {}

    if args.action == "gen":
        if not args.board:
            print("bbtool scaffold gen: error: --board required", file=sys.stderr)
            return 1
        try:
            graph = build_graph(root, args.board, config)
        except ManifestError as e:
            print(f"bbtool scaffold gen: error: {e}", file=sys.stderr)
            return 1
        sys.stdout.write(_format_graph(graph))
        return 0
    return 1


# ---------------------------------------------------------------------------
# PlatformIO / SCons pre-script entry
# ---------------------------------------------------------------------------

def pio_main(env, root: str, board: str, config: dict) -> None:
    """SCons path: resolve the board's derived graph and mutate env
    BUILD_FLAGS / SRC_FILTER — the scaffold replacement for
    native_scaffold.py's COMPONENT_MAP wiring. `env.Exit(1)` on any
    ManifestError, mirroring native_scaffold.py's unknown-component
    behavior."""
    try:
        graph = build_graph(root, board, config)
    except ManifestError as e:
        print(f"bbtool scaffold: error: {e}")
        env.Exit(1)
        return

    for cap in graph["capabilities"]:
        flag = f"-DBB_CAP_{cap.upper()}=1"
        if flag not in env.get("BUILD_FLAGS", []):
            env.Append(BUILD_FLAGS=[flag])

    for name in graph["order"]:
        entry = graph["components"][name]

        for inc_path in entry["includes"]:
            abs_inc = os.path.join(root, inc_path)
            flag = f"-I{abs_inc}"
            if flag not in env.get("BUILD_FLAGS", []):
                env.Append(BUILD_FLAGS=[flag])

        for src_path in entry["sources"]:
            abs_src = os.path.join(root, src_path)
            src_filter_entry = f"+<{abs_src}>"
            current_filter = env.get("SRC_FILTER", "")
            if src_filter_entry not in current_filter:
                env.Append(SRC_FILTER=[src_filter_entry])

        print(f"bb_scaffold: {name} -> {len(entry['sources'])} sources, {len(entry['includes'])} includes")
