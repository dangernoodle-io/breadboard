"""scaffold command tests: `gen` CLI output + the PIO pre-hook env mutation."""
import argparse
import contextlib
import io
import os
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "commands"))

from commands.scaffold import run, pio_main


def _write(path: Path, content: str = "") -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")


def _fixture_root(tmp: str) -> Path:
    root = Path(tmp)
    comp = root / "components" / "bb_core"
    _write(comp / "CMakeLists.txt", "idf_component_register()\n")
    _write(comp / "include" / "bb_core.h", "#pragma once\n")
    _write(root / "platform" / "host" / "bb_core" / "bb_core.c", "// impl\n")
    return root


class _FakeEnv:
    """Minimal stand-in for SCons' env object: dict-backed get()/Append(),
    plus Exit() recording instead of raising SystemExit."""

    def __init__(self):
        self._data = {}
        self.exited_with = None

    def get(self, key, default=None):
        return self._data.get(key, default)

    def Append(self, **kwargs):
        for key, values in kwargs.items():
            self._data.setdefault(key, [])
            self._data[key] = self._data[key] + list(values)

    def Exit(self, code):
        self.exited_with = code


class TestScaffoldGenCli(unittest.TestCase):
    def test_gen_prints_graph_and_returns_zero(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = _fixture_root(tmp)
            config = {
                "capability": {"core": {"required": True, "components": ["bb_core"]}},
                "board": {"native": {"platform": "host"}},
            }
            args = argparse.Namespace(root=str(root), board="native", action="gen",
                                       _config_dict=config, _root_abs=str(root))
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                rc = run(args)
            self.assertEqual(rc, 0)
            out = buf.getvalue()
            self.assertIn("board: native", out)
            self.assertIn("[bb_core]", out)
            self.assertIn("-DBB_CAP_CORE=1", out)

    def test_gen_missing_board_errors(self):
        args = argparse.Namespace(root=os.getcwd(), board=None, action="gen",
                                   _config_dict={}, _root_abs=os.getcwd())
        rc = run(args)
        self.assertEqual(rc, 1)

    def test_gen_unknown_board_errors(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = _fixture_root(tmp)
            args = argparse.Namespace(root=str(root), board="ghost", action="gen",
                                       _config_dict={"board": {}, "capability": {}},
                                       _root_abs=str(root))
            rc = run(args)
            self.assertEqual(rc, 1)


class TestScaffoldPioMain(unittest.TestCase):
    def test_pio_main_wires_includes_sources_and_cap_flags(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = _fixture_root(tmp)
            config = {
                "capability": {"core": {"required": True, "components": ["bb_core"]}},
                "board": {"native": {"platform": "host"}},
            }
            env = _FakeEnv()
            pio_main(env, str(root), "native", config)
            self.assertIsNone(env.exited_with)
            flags = env.get("BUILD_FLAGS", [])
            self.assertIn("-DBB_CAP_CORE=1", flags)
            self.assertIn(f"-I{os.path.join(str(root), 'components/bb_core/include')}", flags)
            src_filter = env.get("SRC_FILTER", [])
            self.assertTrue(
                any("bb_core.c" in entry for entry in src_filter)
            )

    def test_pio_main_idempotent_no_duplicate_flags(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = _fixture_root(tmp)
            config = {
                "capability": {"core": {"required": True, "components": ["bb_core"]}},
                "board": {"native": {"platform": "host"}},
            }
            env = _FakeEnv()
            pio_main(env, str(root), "native", config)
            pio_main(env, str(root), "native", config)
            flags = env.get("BUILD_FLAGS", [])
            self.assertEqual(flags.count("-DBB_CAP_CORE=1"), 1)

            # -I include flags must also dedup, not just -DBB_CAP_* flags.
            include_flag = f"-I{os.path.join(str(root), 'components/bb_core/include')}"
            self.assertEqual(flags.count(include_flag), 1)

            # SRC_FILTER / build_src_filter entries must dedup too.
            src_filter = env.get("SRC_FILTER", []) + env.get("build_src_filter", [])
            core_entries = [e for e in src_filter if "bb_core.c" in e]
            self.assertEqual(len(core_entries), 1)

    def test_pio_main_exits_on_manifest_error(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = _fixture_root(tmp)
            config = {"board": {}, "capability": {}}
            env = _FakeEnv()
            pio_main(env, str(root), "ghost", config)
            self.assertEqual(env.exited_with, 1)


if __name__ == "__main__":
    unittest.main()
