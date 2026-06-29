"""Argparse dispatcher — builds subparsers from the COMMANDS registry."""
from __future__ import annotations
import argparse
import os
import sys

from core import load_config, load_plugins
from registry import COMMANDS, PluginAPI
from commands import FIRST_PARTY


def main() -> int:
    # Pre-parse --root/--config before building subparsers so we can locate
    # bbtool.toml (and thus external plugin paths) before argparse sees the
    # subcommand.  parse_known_args() ignores unknown tokens (the subcommand
    # and its flags) so --root after a subcommand is also handled correctly.
    pre = argparse.ArgumentParser(add_help=False)
    pre.add_argument("--root", default=os.getcwd())
    pre.add_argument("--config", default=None)
    pre_args, _ = pre.parse_known_args()

    root = os.path.abspath(pre_args.root)
    config = load_config(pre_args.config, root)
    config_dir = os.path.dirname(pre_args.config) if pre_args.config else root

    api = PluginAPI()

    # Load first-party commands (and lint rules) onto the bus first.
    for mod in FIRST_PARTY:
        if hasattr(mod, "register"):
            mod.register(api)

    # Load external plugins — they land in the same COMMANDS/RULES dicts.
    plugin_paths = config.get("plugins", {}).get("paths", [])
    load_plugins(plugin_paths, config_dir, api)

    # COMMANDS is now fully populated; build the real parser.
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

    # Resolve root from the full parse.
    root = os.path.abspath(getattr(args, "root", None) or os.getcwd())

    if args.command is None:
        parser.print_help()
        return 1

    if not hasattr(args, "root") or args.root is None:
        args.root = root
    args._config_dict = config
    args._root_abs = root

    return COMMANDS[args.command].run(args)
