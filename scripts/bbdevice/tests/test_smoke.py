"""Smoke tests for the bbdevice package skeleton.

Exercises bbdevice as a proper package: only the PARENT scripts/ dir goes on
sys.path, and everything is imported via absolute `bbdevice.*` paths. If the
absolute-import conversion regresses (any intra-package `from core import ...`
style import), `import bbdevice.cli` below fails and this module errors out —
so the package-style import is genuinely load-bearing here.
"""
from __future__ import annotations
import os
import subprocess
import sys
import types
import unittest
from pathlib import Path

# ONLY scripts/ on sys.path — bbdevice must resolve as a package with its
# internal imports written absolute (`from bbdevice.core import ...`). The
# package dir itself is deliberately NOT added.
_SCRIPTS_DIR = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, _SCRIPTS_DIR)

import bbdevice.cli as cli  # noqa: E402
from bbdevice.registry import COMMANDS, PluginAPI  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parent.parent.parent.parent
BBDEVICE_SCRIPT = REPO_ROOT / "scripts" / "bbdevice.py"


class TestCliHelp(unittest.TestCase):
    """Empty CLI (no commands registered) still builds a parser and --help works."""

    def test_help_exits_zero_in_process(self):
        old_argv = sys.argv
        sys.argv = ["bbdevice", "--help"]
        try:
            with self.assertRaises(SystemExit) as cm:
                cli.main()
            self.assertEqual(cm.exception.code, 0)
        finally:
            sys.argv = old_argv

    def test_help_exits_zero_subprocess(self):
        result = subprocess.run(
            [sys.executable, str(BBDEVICE_SCRIPT), "--help"],
            cwd=str(REPO_ROOT),
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("bbdevice", result.stdout)

    def test_no_command_prints_help_and_exits_nonzero(self):
        result = subprocess.run(
            [sys.executable, str(BBDEVICE_SCRIPT)],
            cwd=str(REPO_ROOT),
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 1, result.stderr)


class TestPluginBus(unittest.TestCase):
    """PluginAPI.add_command lands a dummy command in the COMMANDS registry."""

    def setUp(self):
        self._saved_commands = dict(COMMANDS)

    def tearDown(self):
        COMMANDS.clear()
        COMMANDS.update(self._saved_commands)

    def test_add_command_registers_into_commands(self):
        mod = types.SimpleNamespace(
            NAME="dummy",
            HELP="dummy test command",
            add_arguments=lambda parser: None,
            run=lambda args: 0,
        )
        api = PluginAPI()
        api.add_command("dummy", mod)

        self.assertIn("dummy", COMMANDS)
        self.assertIs(COMMANDS["dummy"], mod)


if __name__ == "__main__":
    unittest.main()
