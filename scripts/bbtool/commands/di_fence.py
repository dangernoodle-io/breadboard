"""di-fence command — thin back-compat alias for `fence --family di_legacy`.

Kept as a separate CLI subcommand (rather than removed) so existing muscle
memory / scripts (`python3 scripts/bbtool.py di-fence`, `make di-fence`)
keep working. All scanning/baseline/diff logic lives in the generalized
`fence` command (`scripts/bbtool/commands/fence_cmd.py`) and the `di_legacy`
fence family (`scripts/bbtool/fence/di_legacy.py`); this module only
translates CLI args and delegates.
"""
from __future__ import annotations
import argparse
import sys

from commands import fence_cmd

NAME = "di-fence"
HELP = "[alias] DI legacy ratchet-fence lint — see `fence --family di_legacy`"

_FAMILY = "di_legacy"


def add_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument(
        "--root",
        default=None,
        help="repository root (default: from top-level --root or cwd)",
    )
    parser.add_argument(
        "--update-baseline",
        action="store_true",
        help="shrink-only: prune the di_legacy baseline (see `fence --help`)",
    )


def run(args: argparse.Namespace) -> int:
    fence_args = argparse.Namespace(
        root=getattr(args, "root", None),
        _root_abs=getattr(args, "_root_abs", None),
        family=[_FAMILY],
        update_baseline=getattr(args, "update_baseline", False),
        seed=None,
    )
    return fence_cmd.run(fence_args)


def register(api) -> None:
    api.add_command(NAME, sys.modules[__name__])
