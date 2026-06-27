"""Argparse dispatcher — builds subparsers from the COMMANDS registry."""
from __future__ import annotations
import argparse
import os
import sys

from core import load_config, load_plugins
from registry import COMMANDS, PluginAPI
import commands  # registers built-in commands as a side-effect


def main() -> int:
    parser = argparse.ArgumentParser(
        prog="bbtool",
        description="breadboard tooling framework",
    )
    parser.add_argument(
        "--root",
        default=os.getcwd(),
        help="repository root (default: cwd)",
    )
    parser.add_argument(
        "--config",
        default=None,
        help="path to bbtool.toml (default: <root>/bbtool.toml)",
    )

    subparsers = parser.add_subparsers(dest="command", metavar="<command>")
    for name, mod in COMMANDS.items():
        sub = subparsers.add_parser(name, help=getattr(mod, "HELP", ""))
        if hasattr(mod, "add_arguments"):
            mod.add_arguments(sub)

    args = parser.parse_args()

    # Resolve root
    root = os.path.abspath(args.root)

    # Load config
    config = load_config(args.config, root)

    # Load plugins
    plugin_paths = config.get("plugins", {}).get("paths", [])
    config_dir = os.path.dirname(args.config) if args.config else root
    api = PluginAPI()
    load_plugins(plugin_paths, config_dir, api)

    if args.command is None:
        parser.print_help()
        return 1

    # Inject root + config into args if not already set
    if not hasattr(args, "root") or args.root is None:
        args.root = root
    args._config_dict = config
    args._root_abs = root

    mod = COMMANDS[args.command]
    return mod.run(args)
