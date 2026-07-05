"""docs command tests: deps parsing, platform matrix, determinism, component-readme rule."""
import io
import os
import sys
import tempfile
import unittest
from contextlib import redirect_stderr
from pathlib import Path

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "commands"))

from core import Context
from commands.docs import (
    _parse_requires,
    _render_deps,
    _platform_matrix,
    _render_platform,
    _rewrite_markers,
    _strip_cmake_comments,
    NestedMarkerError,
    gen_all,
    _check_component_readme,
)


README_TEMPLATE = """# bb_fake

One-line brief.

## Dependencies

<!-- BEGIN bbtool:deps -->
placeholder
<!-- END bbtool:deps -->

## Platform support

<!-- BEGIN bbtool:platform -->
placeholder
<!-- END bbtool:platform -->

## See also

Hand-authored trailer, must survive byte-for-byte.
"""


def _make_component(root: str, name: str, cmake_body: str, readme: str = README_TEMPLATE) -> Path:
    comp = Path(root) / "components" / name
    comp.mkdir(parents=True)
    (comp / "CMakeLists.txt").write_text(cmake_body, encoding="utf-8")
    (comp / "README.md").write_text(readme, encoding="utf-8")
    return comp


class TestParseRequires(unittest.TestCase):
    def test_requires_and_priv_requires(self):
        cmake = (
            "idf_component_register(\n"
            '    SRCS "src/bb_fake.c"\n'
            "    INCLUDE_DIRS \"include\"\n"
            "    REQUIRES bb_core bb_json\n"
            "    PRIV_REQUIRES bb_log esp_timer\n"
            ")\n"
        )
        requires, priv = _parse_requires(cmake)
        self.assertEqual(requires, ["bb_core", "bb_json"])
        self.assertEqual(priv, ["bb_log", "esp_timer"])

    def test_sorted_and_deduped(self):
        cmake = "idf_component_register(REQUIRES bb_json bb_core bb_core)\n"
        requires, priv = _parse_requires(cmake)
        self.assertEqual(requires, ["bb_core", "bb_json"])  # sorted, deduped
        self.assertEqual(priv, [])

    def test_no_requires(self):
        cmake = 'idf_component_register(INCLUDE_DIRS "include")\n'
        requires, priv = _parse_requires(cmake)
        self.assertEqual(requires, [])
        self.assertEqual(priv, [])

    def test_no_register_call(self):
        requires, priv = _parse_requires("# empty file\n")
        self.assertEqual(requires, [])
        self.assertEqual(priv, [])

    def test_comments_do_not_pollute_requires(self):
        # Mirrors bb_info's real CMakeLists.txt shape: an inline comment block
        # sits between REQUIRES and PRIV_REQUIRES, and must not leak words
        # like "provides" or "emission" into the parsed dependency lists.
        cmake = (
            "idf_component_register(\n"
            "    SRCS\n"
            '        "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_info/bb_info.c"\n'
            '    INCLUDE_DIRS "include"\n'
            "    REQUIRES bb_core bb_nv bb_json bb_response\n"
            "    # bb_ota_pull provides bb_ota_pull_heap_ready() for the ota_ready field. The\n"
            "    # *emission* is gated in bb_info.c (BB_INFO_EMIT_OTA_READY) so --gc-sections\n"
            "    # drops bb_ota_pull on boards with no runtime OTA-TLS path. REQUIRES itself\n"
            "    # can't be CONFIG-conditional -- ESP-IDF expands deps before config is loaded.\n"
            "    # bb_cache/bb_event/bb_event_routes: PRIV_REQUIRES because they are not\n"
            "    # exposed in any public header under components/bb_info/include/.\n"
            "    PRIV_REQUIRES bb_http bb_board bb_wifi\n"
            ")\n"
        )
        requires, priv = _parse_requires(cmake)
        self.assertEqual(requires, ["bb_core", "bb_json", "bb_nv", "bb_response"])
        self.assertEqual(priv, ["bb_board", "bb_http", "bb_wifi"])
        for polluted in ("provides", "emission", "gated", "CONFIG-conditional",
                         "exposed", "REQUIRES", "PRIV_REQUIRES", "The", "itself"):
            self.assertNotIn(polluted, requires)
            self.assertNotIn(polluted, priv)

    def test_strip_cmake_comments_preserves_code(self):
        text = 'REQUIRES bb_core # trailing comment\nPRIV_REQUIRES bb_log\n'
        stripped = _strip_cmake_comments(text)
        self.assertNotIn("trailing comment", stripped)
        self.assertIn("REQUIRES bb_core", stripped)
        self.assertIn("PRIV_REQUIRES bb_log", stripped)

    def test_render_deps_empty(self):
        text = _render_deps([], [])
        self.assertIn("_(none)_", text)

    def test_render_deps_nonempty(self):
        text = _render_deps(["bb_core"], ["bb_log"])
        self.assertIn("`bb_core`", text)
        self.assertIn("`bb_log`", text)


class TestPlatformMatrix(unittest.TestCase):
    def test_matrix_reflects_directory_presence(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            (root / "platform" / "host" / "bb_fake").mkdir(parents=True)
            (root / "platform" / "espidf" / "bb_other").mkdir(parents=True)
            matrix = _platform_matrix(root, "bb_fake")
            self.assertEqual(matrix, {"host": True, "espidf": False, "arduino": False})

    def test_render_platform_matrix(self):
        text = _render_platform({"host": True, "espidf": False, "arduino": True})
        self.assertIn("| yes | no | yes |", text)


class TestMarkerRewrite(unittest.TestCase):
    def test_only_marked_region_rewritten(self):
        content = README_TEMPLATE
        new = _rewrite_markers(content, {
            "deps": lambda: "GENERATED DEPS",
            "platform": lambda: "GENERATED PLATFORM",
        })
        self.assertIn("GENERATED DEPS", new)
        self.assertIn("GENERATED PLATFORM", new)
        self.assertIn("Hand-authored trailer, must survive byte-for-byte.", new)
        self.assertIn("# bb_fake", new)
        self.assertNotIn("placeholder", new)

    def test_unknown_marker_key_left_untouched(self):
        content = (
            "<!-- BEGIN bbtool:mystery -->\nkeepme\n<!-- END bbtool:mystery -->\n"
        )
        new = _rewrite_markers(content, {"deps": lambda: "x"})
        self.assertEqual(new, content)

    def test_nested_marker_region_fails_loudly(self):
        content = (
            "<!-- BEGIN bbtool:deps -->\n"
            "outer\n"
            "<!-- BEGIN bbtool:inner -->\n"
            "inner body\n"
            "<!-- END bbtool:inner -->\n"
            "<!-- END bbtool:deps -->\n"
        )
        stderr = io.StringIO()
        with redirect_stderr(stderr):
            with self.assertRaises(NestedMarkerError):
                _rewrite_markers(content, {"deps": lambda: "GENERATED"},
                                  source="components/bb_fake/README.md")
        msg = stderr.getvalue()
        self.assertIn("components/bb_fake/README.md", msg)
        self.assertIn("bbtool:deps", msg)

    def test_missing_end_marker_warns_and_leaves_untouched(self):
        content = "<!-- BEGIN bbtool:deps -->\ndangling, no end marker\n"
        stderr = io.StringIO()
        with redirect_stderr(stderr):
            new = _rewrite_markers(content, {"deps": lambda: "GENERATED"},
                                    source="components/bb_fake/README.md")
        self.assertEqual(new, content)
        msg = stderr.getvalue()
        self.assertIn("components/bb_fake/README.md", msg)
        self.assertIn("bbtool:deps", msg)
        self.assertIn("no matching END", msg)

    def test_crlf_markers_are_rewritten(self):
        content = (
            "# bb_fake\r\n"
            "<!-- BEGIN bbtool:deps -->\r\n"
            "placeholder\r\n"
            "<!-- END bbtool:deps -->\r\n"
        )
        new = _rewrite_markers(content, {"deps": lambda: "GENERATED"})
        self.assertIn("GENERATED", new)
        self.assertNotIn("placeholder", new)


class TestGenAllDeterminism(unittest.TestCase):
    def test_deps_and_platform_correct_and_second_run_no_diff(self):
        with tempfile.TemporaryDirectory() as td:
            comp = _make_component(
                td, "bb_fake",
                "idf_component_register(REQUIRES bb_core PRIV_REQUIRES bb_log)\n",
            )
            (Path(td) / "platform" / "host" / "bb_fake").mkdir(parents=True)

            results = gen_all(td)
            self.assertEqual(len(results), 1)
            path, changed = results[0]
            self.assertTrue(changed)

            content = (comp / "README.md").read_text(encoding="utf-8")
            self.assertIn("`bb_core`", content)
            self.assertIn("`bb_log`", content)
            self.assertIn("| yes | no | no |", content)
            self.assertIn("Hand-authored trailer, must survive byte-for-byte.", content)

            # Second run on unchanged inputs must be a zero-diff no-op.
            snapshot = content
            results2 = gen_all(td)
            self.assertEqual(len(results2), 1)
            _, changed2 = results2[0]
            self.assertFalse(changed2, "second gen run must report no change")
            self.assertEqual(
                (comp / "README.md").read_text(encoding="utf-8"), snapshot,
                "second gen run must produce byte-identical output",
            )

    def test_components_without_readme_are_untouched(self):
        with tempfile.TemporaryDirectory() as td:
            comp = Path(td) / "components" / "bb_bare"
            comp.mkdir(parents=True)
            (comp / "CMakeLists.txt").write_text(
                "idf_component_register(REQUIRES bb_core)\n", encoding="utf-8"
            )
            results = gen_all(td)
            self.assertEqual(results, [])
            self.assertFalse((comp / "README.md").exists())

    def test_no_components_dir(self):
        with tempfile.TemporaryDirectory() as td:
            self.assertEqual(gen_all(td), [])


class TestComponentReadmeRule(unittest.TestCase):
    def test_fires_on_missing_readme(self):
        with tempfile.TemporaryDirectory() as td:
            comp = Path(td) / "components" / "bb_bare"
            comp.mkdir(parents=True)
            (comp / "CMakeLists.txt").write_text("idf_component_register()\n", encoding="utf-8")
            ctx = Context(root=td, config={})
            violations = _check_component_readme(ctx)
            self.assertTrue(violations)
            self.assertIn("bb_bare", violations[0]["path"])

    def test_passes_when_readme_present(self):
        with tempfile.TemporaryDirectory() as td:
            _make_component(td, "bb_documented", "idf_component_register()\n")
            ctx = Context(root=td, config={})
            violations = _check_component_readme(ctx)
            self.assertEqual(violations, [])

    def test_no_components_dir(self):
        with tempfile.TemporaryDirectory() as td:
            ctx = Context(root=td, config={})
            self.assertEqual(_check_component_readme(ctx), [])


if __name__ == "__main__":
    unittest.main()
