"""fetch command — reconcile .breadboard against a pinned version or local override.

Logic (4 branches in priority order):
  1. LOCAL set  → ensure DEST is a symlink to abspath(LOCAL)
  2. DEST is a symlink (prior local build, no LOCAL override now) → leave as-is
  3. DEST/components is a dir AND stamp == VERSION → up to date (noop)
  4. missing or stale → clone at VERSION from REPO
"""
from __future__ import annotations

import os
import shutil
import subprocess

NAME = "fetch"
HELP = "Reconcile .breadboard against a pinned version or local override"

DEFAULT_REPO = "https://github.com/dangernoodle-io/breadboard.git"


def _read_stamp(dest):
    """Read dest/.version and return stripped content, or '' if missing."""
    try:
        with open(os.path.join(dest, ".version")) as fh:
            return fh.read().strip()
    except OSError:
        return ""


def reconcile(dest, version, repo=DEFAULT_REPO, local=None):
    """Reconcile DEST against VERSION.

    Branch 1: LOCAL set  → ensure DEST is a symlink to abspath(LOCAL).
    Branch 2: DEST is a symlink (no LOCAL override) → leave as-is.
    Branch 3: DEST/components exists and stamp matches VERSION → up to date.
    Branch 4: missing or stale → clone from REPO at VERSION.
    """
    if local:
        target = os.path.abspath(local)
        if os.path.islink(dest) and os.readlink(dest) == target:
            print(f"breadboard: .breadboard -> {target} (local, already linked)")
        else:
            if os.path.islink(dest):
                os.unlink(dest)
            elif os.path.exists(dest):
                shutil.rmtree(dest)
            os.symlink(target, dest)
            print(f"breadboard: linked .breadboard -> {target}")
    elif os.path.islink(dest):
        print(f"breadboard: .breadboard symlink -> {os.path.realpath(dest)} (left as-is)")
    elif os.path.isdir(os.path.join(dest, "components")) and _read_stamp(dest) == version:
        print(f"breadboard: .breadboard at {version} (up to date)")
    else:
        stamp_file = os.path.join(dest, ".version")
        if os.path.exists(dest):
            print(f"breadboard: .breadboard does not match {version}; refetching")
            shutil.rmtree(dest)
        subprocess.check_call(["git", "clone", "--depth", "1", "--branch", version, repo, dest])
        with open(stamp_file, "w") as fh:
            fh.write(version + "\n")
        print(f"breadboard: fetched {version}")


# ---------------------------------------------------------------------------
# bbtool command interface
# ---------------------------------------------------------------------------

def add_arguments(parser):
    parser.add_argument("--dest", required=True,
                        help="Path to the .breadboard directory")
    parser.add_argument("--version", required=True,
                        help="breadboard version tag to pin (e.g. v0.70.0)")
    parser.add_argument("--repo", default=DEFAULT_REPO,
                        help=f"Git remote to clone from (default: {DEFAULT_REPO})")
    parser.add_argument("--local", default=os.environ.get("BREADBOARD_LOCAL"),
                        help="Local breadboard checkout to symlink (overrides clone; "
                             "default: $BREADBOARD_LOCAL)")


def run(args):
    """CLI entry: reconcile .breadboard, return 0 on success."""
    try:
        reconcile(dest=args.dest, version=args.version, repo=args.repo, local=args.local)
        return 0
    except Exception as exc:
        print(f"breadboard: error: {exc}")
        return 1
