"""copy_artifacts.py — PlatformIO post: build-artifact staging hook.

Pure glue: translates SCons env → argv and execs `bbtool stage`.
Wire up in platformio.ini:
  extra_scripts =
      pre:../../scripts/bbtool_pio.py
      post:../../scripts/copy_artifacts.py
"""
import inspect
import os
import subprocess
import sys

_THIS_FILE = os.path.abspath(inspect.currentframe().f_code.co_filename)
_SCRIPTS_DIR = os.path.dirname(os.path.realpath(_THIS_FILE))


def _run(source, target, env):
    frameworks = env.get("PIOFRAMEWORK", [])
    if isinstance(frameworks, str):
        frameworks = [frameworks]
    if "espidf" not in frameworks:
        return

    bbtool_py = os.path.join(_SCRIPTS_DIR, "bbtool.py")
    result = subprocess.run(
        [
            sys.executable, bbtool_py, "stage",
            "--build-dir", env.subst("$BUILD_DIR"),
            "--env", env.subst("$PIOENV"),
            "--project-dir", env.subst("$PROJECT_DIR"),
        ],
        check=False,
    )
    if result.returncode != 0:
        print(f"[bbtool] stage exited {result.returncode} (non-fatal)")


try:
    Import("env")  # noqa: F821
    env.AddPostAction("$BUILD_DIR/firmware.bin", _run)  # noqa: F821
except NameError:
    pass
