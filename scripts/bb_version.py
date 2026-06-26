"""
bb_version.py — PlatformIO pre-script that writes a generated C header containing
the firmware version string.

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

Exact commit identity for crash decoding lives in the panic record's
app_sha256 + the elf archive, not in this human-readable string.

The header is written to <PROJECT_DIR>/.breadboard/gen/bb_version_gen.h and
contains:  #define BB_FW_VERSION_STR "<string>"

Only rewritten when the content changes to avoid spurious recompiles.

Consumers must add the generated header directory to their include path:
  build_flags = -I${PROJECT_DIR}/.breadboard/gen
"""

Import("env")

import hashlib
import inspect
import os
import subprocess

# ---------------------------------------------------------------------------
# helpers
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
    dir with a .version stamp): show the pin version ("bb-0.70.3"). Local symlink
    (floating dev checkout): show the ref label ("bb-main" / "bb-<sha>[+hash4]")."""
    dest = os.path.join(consumer_dir, ".breadboard")
    if not os.path.islink(dest):
        try:
            with open(os.path.join(dest, ".version")) as fh:
                pin = fh.read().strip()
        except OSError:
            pin = ""
        pin = pin[1:] if pin.startswith("v") else pin
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
# main
# ---------------------------------------------------------------------------

# Consumer repo = PlatformIO project dir
consumer_dir = env.subst("$PROJECT_DIR")

# breadboard repo = the directory containing this script (resolve symlinks so
# it works whether .breadboard is a symlink or a real directory).
# `__file__` isn't set when SCons exec()'s the script — use inspect to recover
# the absolute path from the current frame's code-object filename.
_script_path = os.path.abspath(inspect.currentframe().f_code.co_filename)
script_dir = os.path.dirname(os.path.realpath(_script_path))
bb_dir = os.path.abspath(os.path.join(script_dir, ".."))

version = _compute_version(consumer_dir, bb_dir)

header_dir = os.path.join(consumer_dir, ".breadboard", "gen")
header_path = os.path.join(header_dir, "bb_version_gen.h")

content = (
    "/* Auto-generated by scripts/bb_version.py — do not edit */\n"
    "#pragma once\n"
    f'#define BB_FW_VERSION_STR "{version}"\n'
)

changed = _write_if_changed(header_path, content)
status = "updated" if changed else "unchanged"
print(f"bb_version: {version} ({status})")

# Inject the generated directory into the build include path so consumer
# C files can use #if __has_include("bb_version_gen.h")
env.Append(CPPPATH=[header_dir])
