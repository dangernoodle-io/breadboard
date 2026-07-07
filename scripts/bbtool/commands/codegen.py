"""codegen command: folds `bbtool wire` and `bbtool autowire` into one
command (previously deferred in commands/wire.py's module docstring).

Resolves the composition's transitive closure exactly ONCE via
`commands.autowire.resolve_composition()`, then emits BOTH artifacts from
that single resolution:

  1. The COMPONENTS link-set fragment (`bb_autowire_components.cmake`) --
     same content `bbtool autowire` produces.
  2. `bb_app_init.c` (+ sibling `.cmake`) wired from `// bbtool:init`
     markers -- same content the (now-folded) `bbtool wire` used to
     produce.

Neither underlying algorithm is reimplemented here: the CMake-fragment
renderer and topo-sort/marker-parsing/source-renderer all still live in
`commands.autowire` / `wire_parse` / `wire_graph` / `commands.wire` and are
simply called into.

`--components` is REQUIRED (mirrors the graduated `bbtool autowire` CLI —
the spike's `--composition`/`COMPOSITIONS` preset shortcut was retired when
autowire graduated out of spike status; there is no equivalent here).
"""
from __future__ import annotations
import argparse
import os
import sys

from boards import ManifestError
from cmake_parse import ConditionalSetError
from commands.autowire import (
    DEFAULT_OUT_REL as AUTOWIRE_DEFAULT_OUT_REL,
    DEFAULT_PLATFORM,
    render_cmake_fragment as render_autowire_fragment,
    resolve_composition,
)
from commands.wire import (
    DEFAULT_OUT_REL as WIRE_DEFAULT_OUT_REL,
    WireError,
    collect_entries,
    render_cmake_fragment as render_wire_cmake_fragment,
    render_source,
)
from wire_graph import CycleError, MissingProviderError, topo_sort
from wire_parse import ParseError

NAME = "codegen"
HELP = "codegen: resolve the composition once, emit both the link-set fragment and bb_app_init.c"


def add_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--root", default=os.getcwd(), help="repository root (default: cwd)")
    parser.add_argument(
        "--components", required=True,
        help="comma-separated component names",
    )
    parser.add_argument(
        "--platform", default=DEFAULT_PLATFORM,
        help=f"platform layer to resolve against (default: {DEFAULT_PLATFORM})",
    )
    parser.add_argument(
        "--components-out", default=None,
        help=f"output link-set .cmake path (default: <root>/{AUTOWIRE_DEFAULT_OUT_REL})",
    )
    parser.add_argument(
        "--wire-out", default=None,
        help=f"output bb_app_init.c path (default: <root>/{WIRE_DEFAULT_OUT_REL}); a "
             "sibling '.cmake' is written alongside it",
    )


def register(api) -> None:
    api.add_command(NAME, sys.modules[__name__])


def run(args: argparse.Namespace) -> int:
    root = os.path.abspath(getattr(args, "root", None) or os.getcwd())

    names = [n.strip() for n in (args.components or "").split(",") if n.strip()]
    if not names:
        print("bbtool codegen: error: --components must list at least one component", file=sys.stderr)
        return 1

    try:
        # Single resolution, shared by both artifacts below.
        components = resolve_composition(root, names, args.platform)

        entries = collect_entries(root, components, args.platform)
        ordered = topo_sort(entries)
        source = render_source(ordered)
    except (ManifestError, ConditionalSetError, ParseError, CycleError, MissingProviderError, WireError) as e:
        print(f"bbtool codegen: error: {e}", file=sys.stderr)
        return 1

    components_out = args.components_out or os.path.join(root, AUTOWIRE_DEFAULT_OUT_REL)
    os.makedirs(os.path.dirname(components_out), exist_ok=True)
    with open(components_out, "w", encoding="utf-8") as f:
        f.write(render_autowire_fragment(components))

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
