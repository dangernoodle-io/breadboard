"""autowire command tests: component-list-to-REQUIRES transitive resolution +
CMake fragment rendering, over synthetic CMakeLists.txt fixtures (never the
real breadboard component tree) — mirrors test_boards.py's fixture style."""
import argparse
import io
import contextlib
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "commands"))

from boards import ManifestError
from cmake_parse import ConditionalSetError
from commands.autowire import (
    render_cmake_fragment,
    resolve_composition,
    run,
)


def _write(path: Path, content: str = "") -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")


def _make_component(root: Path, name: str, requires=None, priv_requires=None) -> None:
    body = "idf_component_register(\n"
    if requires:
        body += f"    REQUIRES {' '.join(requires)}\n"
    if priv_requires:
        body += f"    PRIV_REQUIRES {' '.join(priv_requires)}\n"
    body += ")\n"
    comp = root / "components" / name
    _write(comp / "CMakeLists.txt", body)
    _write(comp / "include" / f"{name}.h", "#pragma once\n")


def _fixture_root(tmp: str) -> Path:
    """bb_a <- bb_b <- bb_c chain, plus an independent bb_d, mirroring a small
    slice of the real dependency shape (e.g. bb_wifi -> bb_core)."""
    root = Path(tmp)
    _make_component(root, "bb_a")
    _make_component(root, "bb_b", requires=["bb_a"])
    _make_component(root, "bb_c", priv_requires=["bb_b"])
    _make_component(root, "bb_d")
    return root


class TestResolveComposition(unittest.TestCase):
    def test_resolves_transitive_closure_dependency_before_dependent(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = _fixture_root(tmp)
            order = resolve_composition(str(root), ["bb_c"], platform="espidf")
            self.assertEqual(order, ["bb_a", "bb_b", "bb_c"])

    def test_excludes_components_outside_the_requested_closure(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = _fixture_root(tmp)
            order = resolve_composition(str(root), ["bb_a"], platform="espidf")
            self.assertEqual(order, ["bb_a"])
            self.assertNotIn("bb_b", order)
            self.assertNotIn("bb_c", order)
            self.assertNotIn("bb_d", order)

    def test_unknown_component_raises_manifest_error(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = _fixture_root(tmp)
            with self.assertRaises(ManifestError):
                resolve_composition(str(root), ["bb_ghost"], platform="espidf")

    def test_dedupes_shared_transitive_deps_across_multiple_requested(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = _fixture_root(tmp)
            order = resolve_composition(str(root), ["bb_b", "bb_c"], platform="espidf")
            self.assertEqual(order.count("bb_a"), 1)
            self.assertEqual(order.count("bb_b"), 1)
            self.assertEqual(set(order), {"bb_a", "bb_b", "bb_c"})


class TestRenderCmakeFragment(unittest.TestCase):
    def test_emits_set_with_space_separated_list(self):
        text = render_cmake_fragment(["bb_a", "bb_b", "bb_c"])
        self.assertIn("set(BB_AUTOWIRE_REQUIRES bb_a bb_b bb_c)", text)

    def test_empty_composition_emits_empty_set(self):
        text = render_cmake_fragment([])
        self.assertIn("set(BB_AUTOWIRE_REQUIRES )", text)

    def test_emits_components_allowlist_prefixed_with_main(self):
        text = render_cmake_fragment(["bb_a", "bb_b", "bb_c"])
        self.assertIn("set(BB_AUTOWIRE_COMPONENTS main bb_a bb_b bb_c)", text)

    def test_empty_composition_components_is_just_main(self):
        text = render_cmake_fragment([])
        self.assertIn("set(BB_AUTOWIRE_COMPONENTS main )", text)


class TestRunCli(unittest.TestCase):
    def test_run_writes_fragment_and_returns_zero(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = _fixture_root(tmp)
            out_path = str(root / "out" / "bb_autowire_components.cmake")
            args = argparse.Namespace(
                root=str(root), components="bb_c",
                platform="espidf", out=out_path,
            )
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                rc = run(args)
            self.assertEqual(rc, 0)
            self.assertTrue(os.path.isfile(out_path))
            content = Path(out_path).read_text(encoding="utf-8")
            self.assertIn("set(BB_AUTOWIRE_REQUIRES bb_a bb_b bb_c)", content)

    def test_run_writes_fragment_when_out_has_no_directory_component(self):
        # Regression: os.path.dirname("foo.cmake") == "" -- os.makedirs("")
        # raises FileNotFoundError if not guarded.
        with tempfile.TemporaryDirectory() as tmp:
            root = _fixture_root(tmp)
            cwd = os.getcwd()
            os.chdir(tmp)
            try:
                args = argparse.Namespace(
                    root=str(root), components="bb_c",
                    platform="espidf", out="bare_out.cmake",
                )
                buf = io.StringIO()
                with contextlib.redirect_stdout(buf):
                    rc = run(args)
                self.assertEqual(rc, 0)
                self.assertTrue(os.path.isfile(os.path.join(tmp, "bare_out.cmake")))
            finally:
                os.chdir(cwd)

    def test_run_requires_nonempty_components(self):
        args = argparse.Namespace(root=os.getcwd(), components="",
                                   platform="espidf", out=None)
        rc = run(args)
        self.assertEqual(rc, 1)

    def test_run_unknown_component_returns_error(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = _fixture_root(tmp)
            args = argparse.Namespace(root=str(root), components="bb_ghost",
                                       platform="espidf", out=None)
            rc = run(args)
            self.assertEqual(rc, 1)

    def test_run_reports_conditional_set_error_as_clean_cli_error(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = _fixture_root(tmp)
            args = argparse.Namespace(root=str(root), components="bb_c",
                                       platform="espidf", out=None)
            with mock.patch(
                "commands.autowire.resolve_composition",
                side_effect=ConditionalSetError("REQUIRES fed by a variable set() inside if()/endif()"),
            ):
                buf = io.StringIO()
                with contextlib.redirect_stderr(buf):
                    rc = run(args)
                self.assertEqual(rc, 1)
                self.assertIn("bbtool autowire: error:", buf.getvalue())


if __name__ == "__main__":
    unittest.main()
