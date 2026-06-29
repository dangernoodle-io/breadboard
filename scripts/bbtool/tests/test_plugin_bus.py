"""Plugin bus tests: load-order fix, first-party registration, unified RULES."""
from __future__ import annotations
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

REPO_ROOT = Path(__file__).resolve().parent.parent.parent.parent
BBTOOL_SCRIPT = REPO_ROOT / "scripts" / "bbtool.py"


def _run(*args):
    return subprocess.run(
        [sys.executable, str(BBTOOL_SCRIPT)] + list(args),
        cwd=str(REPO_ROOT),
        capture_output=True,
        text=True,
    )


class TestBuiltinCommandDispatch(unittest.TestCase):
    """All 5 built-in commands resolve and dispatch through the unified bus."""

    def test_lint_dispatches(self):
        r = _run("lint", "--list")
        self.assertEqual(r.returncode, 0, r.stderr)

    def test_version_dispatches(self):
        r = _run("version", "--emit", "--consumer", str(REPO_ROOT), "--bb-dir", str(REPO_ROOT))
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertTrue(r.stdout.strip(), "version must print a non-empty string")

    def test_embed_dispatches(self):
        r = _run("embed", "--help")
        self.assertEqual(r.returncode, 0, r.stderr)

    def test_gen_site_dispatches(self):
        r = _run("gen-site", "--help")
        self.assertEqual(r.returncode, 0, r.stderr)

    def test_fetch_dispatches(self):
        r = _run("fetch", "--help")
        self.assertEqual(r.returncode, 0, r.stderr)


class TestBuiltinLintRulesOnBus(unittest.TestCase):
    """Built-in lint rules land in the unified RULES dict after register(api)."""

    def setUp(self):
        from registry import COMMANDS, RULES
        self._saved_commands = dict(COMMANDS)
        self._saved_rules = dict(RULES)
        COMMANDS.clear()
        RULES.clear()

    def tearDown(self):
        from registry import COMMANDS, RULES
        COMMANDS.clear()
        COMMANDS.update(self._saved_commands)
        RULES.clear()
        RULES.update(self._saved_rules)

    def test_builtin_rules_in_unified_rules(self):
        from registry import RULES, PluginAPI
        from commands.lint import _LINT_RULES
        import commands.lint as lint_mod

        api = PluginAPI()
        lint_mod.register(api)

        for rule_id in _LINT_RULES:
            self.assertIn(rule_id, RULES, f"built-in rule '{rule_id}' missing from RULES")

    def test_external_plugin_rule_lands_and_fires(self):
        """External add_rule lands in RULES alongside built-in rules and fires."""
        from registry import RULES, RULES as _RULES, PluginAPI, Rule
        from core import Context
        import commands.lint as lint_mod

        api = PluginAPI()
        lint_mod.register(api)

        fired = []

        def check_fn(ctx):
            fired.append(True)
            return []

        ext_rule = Rule(
            id="ext-test-rule",
            default_severity="error",
            profiles={"all"},
            check=check_fn,
            hint="ext hint",
        )
        api.add_rule(ext_rule)

        self.assertIn("ext-test-rule", RULES)
        self.assertIn("deprecated-http-send", RULES)

        # fire the external rule
        with tempfile.TemporaryDirectory() as td:
            ctx = Context(root=td, config={})
            RULES["ext-test-rule"].check(ctx)
        self.assertTrue(fired, "external rule check must be callable via RULES")


class TestExternalPluginCommandDispatch(unittest.TestCase):
    """Regression: external plugin add_command dispatches via CLI.

    Before the load-order fix, load_plugins() ran AFTER parse_args(), so
    add_command() registered too late — the subparser didn't exist and bbtool
    would exit with 'error: argument <command>: invalid choice'.
    """

    def test_plugin_add_command_dispatches(self):
        with tempfile.TemporaryDirectory() as td:
            plugin_py = Path(td) / "hello_plugin.py"
            plugin_py.write_text(
                "import types\n\n"
                "def register(api):\n"
                "    mod = types.SimpleNamespace(\n"
                "        NAME='hello',\n"
                "        HELP='say hello',\n"
                "        add_arguments=lambda p: None,\n"
                "        run=lambda args: 0,\n"
                "    )\n"
                "    api.add_command('hello', mod)\n"
            )
            toml_path = Path(td) / "bbtool.toml"
            toml_path.write_text(f'[plugins]\npaths = ["{plugin_py}"]\n')

            r = _run("--root", td, "--config", str(toml_path), "hello")
            self.assertEqual(
                r.returncode,
                0,
                f"plugin-registered subcommand must dispatch (rc={r.returncode})\nstderr={r.stderr}",
            )


if __name__ == "__main__":
    unittest.main()
