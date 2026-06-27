"""Framework unit tests: severity off, profile filtering, plugin loading."""
import os
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "commands"))

from core import Context, load_config, load_plugins
from registry import Rule, COMMANDS, RULES, PluginAPI
from commands.lint import _LINT_RULES, run as lint_run
import argparse


class TestSeverityOff(unittest.TestCase):
    """severity="off" in config suppresses a rule."""

    def test_severity_off_suppresses_rule(self):
        with tempfile.TemporaryDirectory() as td:
            # Create a violation
            src = Path(td) / "components" / "bb_fake" / "src"
            src.mkdir(parents=True)
            (src / "fake.c").write_text(
                'void f(void) { bb_http_resp_send_json(r, doc); }\n'
            )
            # Config with severity=off for that rule
            config = {"lint": {"rules": {"deprecated-http-send": {"severity": "off"}}}}
            args = argparse.Namespace(
                root=td,
                profile="library",
                rules=None,
                list=False,
                _config_dict=config,
                _root_abs=td,
            )
            rc = lint_run(args)
            self.assertEqual(rc, 0, "severity=off must suppress the rule")


class TestProfileFiltering(unittest.TestCase):
    """--profile consumer skips library-profile rules; --profile library runs all."""

    def test_consumer_skips_library_rules(self):
        """consumer profile: public-header-leak (library profile) must NOT run."""
        with tempfile.TemporaryDirectory() as td:
            # Create a public-header-leak violation
            inc = Path(td) / "components" / "bb_fake" / "include"
            inc.mkdir(parents=True)
            (inc / "bb_fake.h").write_text('#pragma once\n#include "esp_http_server.h"\n')
            args = argparse.Namespace(
                root=td,
                profile="consumer",
                rules=None,
                list=False,
                _config_dict={},
                _root_abs=td,
            )
            rc = lint_run(args)
            self.assertEqual(rc, 0, "consumer profile must skip library-profile rules")

    def test_library_runs_all_profiles(self):
        """library profile: public-header-leak MUST fire."""
        with tempfile.TemporaryDirectory() as td:
            inc = Path(td) / "components" / "bb_fake" / "include"
            inc.mkdir(parents=True)
            (inc / "bb_fake.h").write_text('#pragma once\n#include "esp_http_server.h"\n')
            args = argparse.Namespace(
                root=td,
                profile="library",
                rules=None,
                list=False,
                _config_dict={},
                _root_abs=td,
            )
            rc = lint_run(args)
            self.assertEqual(rc, 1, "library profile must run public-header-leak")


class TestPluginLoading(unittest.TestCase):
    """A consumer plugin written to a tmp file, wired via config [plugins] paths, registers a rule that fires."""

    def test_plugin_registers_and_fires(self):
        with tempfile.TemporaryDirectory() as td:
            # Write a plugin that registers a rule firing on any .c file named "trigger.c"
            plugin_src = '''
from registry import Rule

def register(api):
    def check(ctx):
        violations = []
        for path in ctx.files(["**/*.c"], exclude_dirs=[".pio"]):
            if path.name == "trigger.c":
                violations.append(ctx.violation(path, 1, "plugin fired"))
        return violations

    api.add_rule(Rule(
        id="test-plugin-rule",
        default_severity="error",
        profiles={"all"},
        check=check,
        hint="plugin test hint",
    ))
'''
            plugin_file = Path(td) / "test_plugin.py"
            plugin_file.write_text(plugin_src)

            # Create trigger file
            src_dir = Path(td) / "components" / "bb_fake" / "src"
            src_dir.mkdir(parents=True)
            (src_dir / "trigger.c").write_text("void f(void) {}\n")

            # Load plugin via plugin API directly
            from registry import RULES as _RULES
            # Save current RULES state and restore after
            old_rules = dict(_RULES)
            try:
                api = PluginAPI()
                load_plugins([str(plugin_file)], td, api)

                # Manually add plugin rules to RULES for this test
                # (In normal flow, PluginAPI.add_rule writes to RULES)
                # But PluginAPI.add_rule already writes to RULES dict directly
                self.assertIn("test-plugin-rule", _RULES, "plugin must register its rule")

                ctx = Context(root=td, config={})
                violations = _RULES["test-plugin-rule"].check(ctx)
                self.assertTrue(violations, "plugin rule must fire on trigger.c")
            finally:
                # Restore RULES
                _RULES.clear()
                _RULES.update(old_rules)


if __name__ == "__main__":
    unittest.main()
