"""version command — compute and emit breadboard firmware version string.

Precedence (highest → lowest):
  1. BB_FW_VERSION env var (non-empty)  → used verbatim
  2. Consumer repo has an exact git tag at HEAD  → use that tag (release build)
  3. dev default: dev-<tm-ref>-<bb-ref>
       tm-ref : "main" on the main branch, else the 7-char short sha (feature
                branches are transient; the sha is precise). A "+<hash4>" suffix
                (hash of the uncommitted diff) marks a dirty tree so two dirty
                checkouts at the same sha stay distinguishable.
       bb-ref : "bb-<pin>" when .breadboard is a pinned fetch (the version is
                derivable from the consumer commit's pin, so name it), or
                "bb-main" / "bb-<sha>[+hash4]" when .breadboard is a local symlink
                (a floating dev checkout — sha/branch is not derivable).
     e.g.  dev-main-bb-0.70.3   dev-main-bb-main   dev-806bf94+a1b2-bb-main

The generated header content string matches the original bbtool_pio.py comment
verbatim to avoid spurious header rewrites on consumers.
"""
from __future__ import annotations

import hashlib
import inspect
import os
import re
import subprocess
import sys

NAME = "version"
HELP = "Compute and emit breadboard firmware version"

# Capture this file's real path at module load time (handles SCons exec()).
_THIS_FILE = os.path.abspath(inspect.currentframe().f_code.co_filename)
_THIS_DIR = os.path.dirname(os.path.realpath(_THIS_FILE))

# ---------------------------------------------------------------------------
# git helpers
# ---------------------------------------------------------------------------

def _run_git(args, cwd):
    """Run a git subcommand in cwd; return stripped stdout or '' on any error."""
    try:
        result = subprocess.run(
            ["git"] + args,
            cwd=cwd,
            capture_output=True,
            text=True,
            timeout=10,
        )
        return result.stdout.strip() if result.returncode == 0 else ""
    except Exception:
        return ""


def _short_sha(cwd):
    return _run_git(["rev-parse", "--short=7", "HEAD"], cwd)


def _branch(cwd):
    """Current branch name, or '' for detached HEAD / no git."""
    b = _run_git(["rev-parse", "--abbrev-ref", "HEAD"], cwd)
    return "" if b == "HEAD" else b


def _diff_hash(cwd):
    """Short hash of the uncommitted tracked diff, or '' when clean."""
    diff = _run_git(["diff", "HEAD"], cwd)
    if not diff:
        return ""
    return hashlib.sha256(diff.encode("utf-8", "replace")).hexdigest()[:4]


def _exact_tag(cwd):
    """Return the exact tag at HEAD, or '' if none."""
    return _run_git(["describe", "--tags", "--exact-match", "HEAD"], cwd)


def _ref_label(cwd):
    """Readable ref for a dev build: "main" on the main branch, else the short
    sha; a "+<hash4>" suffix marks an uncommitted tree. "" only when git is
    unavailable."""
    branch = _branch(cwd)
    base = "main" if branch == "main" else _short_sha(cwd)
    if not base:
        return ""
    h = _diff_hash(cwd)
    return f"{base}+{h}" if h else base


def _bb_ref(consumer_dir, bb_dir):
    """breadboard identifier for a dev build. Pinned fetch (.breadboard is a real
    dir with a .version stamp): show the pin version ("bb-0.70.3" or "bb-596190b"
    for a full 40-char SHA, truncated to 7 chars). Local symlink (floating dev
    checkout): show the ref label ("bb-main" / "bb-<sha>[+hash4]")."""
    dest = os.path.join(consumer_dir, ".breadboard")
    if not os.path.islink(dest):
        try:
            with open(os.path.join(dest, ".version")) as fh:
                pin = fh.read().strip()
        except OSError:
            pin = ""
        pin = pin[1:] if pin.startswith("v") else pin
        # Truncate full 40-char lowercase hex SHA to 7-char short form
        if re.fullmatch(r"[0-9a-f]{40}", pin):
            pin = pin[:7]
        return "bb-" + (pin or "unknown")
    return "bb-" + (_ref_label(bb_dir) or "unknown")


def _compute_version(consumer_dir, bb_dir):
    """Compute version string according to precedence rules (see module docstring)."""

    # 1. User override
    override = os.environ.get("BB_FW_VERSION", "")
    if override:
        return override

    # 2. Exact tag at consumer HEAD → release build (no bb suffix).
    tag = _exact_tag(consumer_dir)
    if tag:
        return tag

    # 3. Dev default: dev-<tm-ref>-<bb-ref>
    tm = _ref_label(consumer_dir) or "unknown"
    return f"dev-{tm}-{_bb_ref(consumer_dir, bb_dir)}"


def _write_if_changed(path, content):
    """Write content to path only when it differs from the existing file."""
    try:
        with open(path) as fh:
            if fh.read() == content:
                return False
    except OSError:
        pass
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as fh:
        fh.write(content)
    return True


# ---------------------------------------------------------------------------
# bbtool command interface
# ---------------------------------------------------------------------------

def add_arguments(parser):
    parser.add_argument("--emit", action="store_true", required=True,
                        help="Print the version string to stdout")
    parser.add_argument("--consumer", required=True,
                        help="Consumer repo directory (equivalent to $PROJECT_DIR)")
    parser.add_argument("--bb-dir",
                        help="breadboard repo root (default: derived from this file's location)")


def register(api) -> None:
    import sys
    api.add_command(NAME, sys.modules[__name__])


def run(args):
    """CLI entry: print bare version string to stdout, return 0."""
    if args.bb_dir:
        bb_dir = os.path.abspath(args.bb_dir)
    else:
        # scripts/bbtool/commands/ -> ../../../ = repo root
        bb_dir = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))

    consumer_dir = os.path.abspath(args.consumer)
    version = _compute_version(consumer_dir, bb_dir)
    print(version)
    return 0


# ---------------------------------------------------------------------------
# PlatformIO / SCons pre-script entry
# ---------------------------------------------------------------------------

def pio_main(env):
    """SCons path: write the generated version header and inject include path."""
    # Consumer repo = PlatformIO project dir
    consumer_dir = env.subst("$PROJECT_DIR")

    # breadboard repo = scripts/bbtool/commands/ -> ../../../ = repo root.
    # Use _THIS_DIR captured at module load time (inspect-based, handles SCons exec).
    bb_dir = os.path.abspath(os.path.join(_THIS_DIR, "..", "..", ".."))

    version = _compute_version(consumer_dir, bb_dir)

    header_dir = os.path.join(consumer_dir, ".breadboard", "gen")
    header_path = os.path.join(header_dir, "bb_version_gen.h")

    content = (
        "/* Auto-generated by scripts/bbtool_pio.py — do not edit */\n"
        "#pragma once\n"
        f'#define BB_FW_VERSION_STR "{version}"\n'
    )

    changed = _write_if_changed(header_path, content)
    status = "updated" if changed else "unchanged"
    print(f"bb_version: {version} ({status})")

    # Inject the generated directory into the build include path so consumer
    # C files can use #if __has_include("bb_version_gen.h")
    env.Append(CPPPATH=[header_dir])
