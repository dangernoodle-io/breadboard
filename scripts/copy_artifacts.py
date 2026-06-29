"""copy_artifacts.py — PlatformIO post: build-artifact staging hook.

Pure glue: all logic lives in scripts/bbtool/commands/stage.py.
Wire up in platformio.ini:
  extra_scripts =
      pre:../../scripts/bbtool_pio.py
      post:../../scripts/copy_artifacts.py
"""
import inspect
import os
import sys

_THIS_FILE = os.path.abspath(inspect.currentframe().f_code.co_filename)
_THIS_DIR = os.path.dirname(os.path.realpath(_THIS_FILE))
_BBTOOL_DIR = os.path.join(_THIS_DIR, "bbtool")

if _BBTOOL_DIR not in sys.path:
    sys.path.insert(0, _BBTOOL_DIR)

from commands.stage import stage_artifacts  # noqa: E402


def _run(source, target, env):
    frameworks = env.get("PIOFRAMEWORK", [])
    if isinstance(frameworks, str):
        frameworks = [frameworks]
    if "espidf" not in frameworks:
        return
    result = stage_artifacts(
        build_dir=env.subst("$BUILD_DIR"),
        pioenv=env.subst("$PIOENV"),
        project_dir=env.subst("$PROJECT_DIR"),
    )
    if result.get("skipped"):
        print(f"[bbtool] stage skipped: {result.get('reason')}")
    else:
        for path in result.get("staged", []):
            print(f"[bbtool] staged: {path}")
        if result.get("archived"):
            print(f"[bbtool] elf archived: {result['archived'][:16]}…")


try:
    Import("env")  # noqa: F821
    env.AddPostAction("$BUILD_DIR/firmware.bin", _run)  # noqa: F821
except NameError:
    pass
