"""copy_artifacts.py — PlatformIO post: build-artifact staging hook.

Wires up as a post: extra_script in ESP-IDF envs:

  extra_scripts =
      pre:../../scripts/bbtool_pio.py
      post:../../scripts/copy_artifacts.py

On each ESP-IDF build this hook:
  1. Archives firmware.elf into the ELF store (see elfstore.py).
     Archive root: BBTOOL_ELF_ARCHIVE env var > bbtool.toml [elf] archive_dir > ~/.bb/elf-archive/
  2. Copies firmware.{bin,elf} → <project_dir>/dist/<PIOENV>/firmware-<PIOENV>-<version>.{bin,elf}
     (version computed the same way as the pre: version hook).

Arduino envs (r4_wifis3, uno_cc3000) produce no firmware.bin — skipped cleanly.
"""
import inspect
import os
import shutil
import sys

# Resolve paths at module load time (SCons exec() doesn't set __file__).
_THIS_FILE = os.path.abspath(inspect.currentframe().f_code.co_filename)
_THIS_DIR = os.path.dirname(os.path.realpath(_THIS_FILE))  # breadboard/scripts/
_BBTOOL_DIR = os.path.join(_THIS_DIR, "bbtool")


def _ensure_path():
    """Ensure bbtool dir is in sys.path — call before every import."""
    if _BBTOOL_DIR not in sys.path:
        sys.path.insert(0, _BBTOOL_DIR)


def _run(source, target, env):
    """Post-action: archive ELF + stage dist artifacts."""
    frameworks = env.get("PIOFRAMEWORK", [])
    if isinstance(frameworks, str):
        frameworks = [frameworks]
    if "espidf" not in frameworks:
        return

    build_dir = env.subst("$BUILD_DIR")
    project_dir = env.subst("$PROJECT_DIR")
    pioenv = env.subst("$PIOENV")

    bin_path = os.path.join(build_dir, "firmware.bin")
    if not os.path.exists(bin_path):
        print("bb copy_artifacts: firmware.bin absent — skip")
        return

    elf_path = os.path.join(build_dir, "firmware.elf")

    # Compute version using the same logic as the pre: version hook.
    try:
        _ensure_path()
        from commands.version import _compute_version
        bb_dir = os.path.abspath(os.path.join(_THIS_DIR, ".."))
        version = _compute_version(project_dir, bb_dir)
    except Exception as exc:
        version = "unknown"
        print(f"bb copy_artifacts: version compute failed: {exc}")

    # Archive ELF (auto-retention runs inside archive()).
    if os.path.exists(elf_path):
        try:
            _ensure_path()
            import elfstore as _es
            key = _es.archive(elf_path)
            print(f"bb copy_artifacts: elf archived ({key[:16]}…)")
        except Exception as exc:
            import traceback
            print(f"bb copy_artifacts: elf archive failed: {exc}")
            traceback.print_exc()

    # Stage dist artifacts.
    dist_dir = os.path.join(project_dir, "dist", pioenv)
    os.makedirs(dist_dir, exist_ok=True)
    for ext in ("bin", "elf"):
        src = os.path.join(build_dir, f"firmware.{ext}")
        if not os.path.exists(src):
            continue
        dst_name = f"firmware-{pioenv}-{version}.{ext}"
        dst = os.path.join(dist_dir, dst_name)
        shutil.copy2(src, dst)
        print(f"bb copy_artifacts: staged dist/{pioenv}/{dst_name}")


_ensure_path()

try:
    Import("env")  # noqa: F821
    env.AddPostAction("$BUILD_DIR/firmware.bin", _run)  # noqa: F821
except NameError:
    pass
