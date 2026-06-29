"""stage command — artifact staging SSOT for breadboard post-build hooks.

stage_artifacts() is the single source of truth for:
  - firmware naming: firmware-<env>-<version>.{bin,elf}
  - dist layout: <dist_root>/<pioenv>/...
  - ELF archiving (delegated to elfstore)

Import-clean: library path has no CLI/registry dependencies (lazy imports only).
"""
from __future__ import annotations

import inspect
import os
import shutil
import sys
from typing import Optional

NAME = "stage"
HELP = "Stage build artifacts into dist/ with canonical naming"

# Capture real path at module load time (handles SCons exec()).
_THIS_FILE = os.path.abspath(inspect.currentframe().f_code.co_filename)
_THIS_DIR = os.path.dirname(os.path.realpath(_THIS_FILE))  # .../commands/
_BBTOOL_DIR = os.path.dirname(_THIS_DIR)  # .../bbtool/ — elfstore lives here


def _ensure_bbtool_path() -> None:
    if _BBTOOL_DIR not in sys.path:
        sys.path.insert(0, _BBTOOL_DIR)


def stage_artifacts(
    build_dir: str,
    pioenv: str,
    project_dir: str,
    *,
    archive_dir: Optional[str] = None,
    archive: bool = True,
    dist_root: Optional[str] = None,
    version: Optional[str] = None,
) -> dict:
    """Stage firmware artifacts into dist/ with canonical naming.

    Returns dict with keys: skipped, version, archived, staged.
    """
    bin_path = os.path.join(build_dir, "firmware.bin")
    if not os.path.exists(bin_path):
        return {"skipped": True, "reason": "firmware.bin not found"}

    if version is None:
        from commands.version import _compute_version
        # commands/stage.py -> ../../../ = bb root
        bb_root = os.path.abspath(os.path.join(_THIS_DIR, "..", "..", ".."))
        version = _compute_version(project_dir, bb_root)

    archived_key = None
    if archive:
        _ensure_bbtool_path()
        import elfstore as _es
        elf_path = os.path.join(build_dir, "firmware.elf")
        if os.path.exists(elf_path):
            archived_key = _es.archive(elf_path, archive_dir=archive_dir)

    root = dist_root if dist_root else os.path.join(project_dir, "dist")
    dist_dir = os.path.join(root, pioenv)
    os.makedirs(dist_dir, exist_ok=True)

    staged = []
    for ext in ("bin", "elf"):
        src = os.path.join(build_dir, f"firmware.{ext}")
        if not os.path.exists(src):
            continue
        dst_name = f"firmware-{pioenv}-{version}.{ext}"
        dst = os.path.join(dist_dir, dst_name)
        shutil.copy2(src, dst)
        staged.append(dst)

    return {
        "skipped": False,
        "version": version,
        "archived": archived_key,
        "staged": staged,
    }


# ---------------------------------------------------------------------------
# bbtool command interface
# ---------------------------------------------------------------------------

def add_arguments(parser) -> None:
    parser.add_argument("--build-dir", required=True, dest="build_dir",
                        metavar="DIR", help="build directory containing firmware.bin/.elf")
    parser.add_argument("--env", required=True, dest="pioenv",
                        metavar="NAME", help="PlatformIO environment name")
    parser.add_argument("--project-dir", required=True, dest="project_dir",
                        metavar="DIR", help="project root directory")
    parser.add_argument("--archive-dir", dest="archive_dir", default=None,
                        metavar="DIR", help="ELF archive root (overrides BBTOOL_ELF_ARCHIVE)")
    parser.add_argument("--no-archive", dest="archive", action="store_false",
                        help="skip ELF archiving")
    parser.add_argument("--dist-root", dest="dist_root", default=None,
                        metavar="DIR", help="dist root (default: <project-dir>/dist)")


def register(api) -> None:
    api.add_command(NAME, sys.modules[__name__])


def run(args) -> int:
    result = stage_artifacts(
        build_dir=args.build_dir,
        pioenv=args.pioenv,
        project_dir=args.project_dir,
        archive_dir=getattr(args, "archive_dir", None),
        archive=getattr(args, "archive", True),
        dist_root=getattr(args, "dist_root", None),
    )
    if result.get("skipped"):
        print(f"[bbtool stage] skipped: {result.get('reason')}")
        return 0
    print(f"[bbtool stage] version: {result['version']}")
    if result.get("archived"):
        print(f"[bbtool stage] elf archived: {result['archived'][:16]}…")
    for path in result.get("staged", []):
        print(f"[bbtool stage] staged: {path}")
    return 0
