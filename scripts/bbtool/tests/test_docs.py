"""docs command tests: deps parsing, brief/api regions, platform matrix, determinism,
component-readme rule."""
import io
import json
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
    _dep_role_and_link,
    _extract_first_sentence,
    _render_api,
    _render_brief,
    DocsGenError,
    _platform_matrix,
    _render_platform,
    _render_links,
    _self_wiki_link,
    _dedupe_links,
    _render_wiring,
    _render_budget,
    _rewrite_markers,
    _strip_cmake_comments,
    NestedMarkerError,
    gen_all,
    _check_component_readme,
    scaffold_component,
    _component_prefix,
    _cmd_gen,
)
from templating import find_dangling_tokens


README_TEMPLATE = """# bb_fake

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

# Full-shape template mirroring templates/component-readme.md — used by
# tests exercising the brief/api regions.
FULL_TEMPLATE = """# bb_fake

<!-- BEGIN bbtool:brief -->
placeholder
<!-- END bbtool:brief -->

## Public API

<!-- BEGIN bbtool:api -->
placeholder
<!-- END bbtool:api -->

## Dependencies

<!-- BEGIN bbtool:deps -->
placeholder
<!-- END bbtool:deps -->

## Platform support

<!-- BEGIN bbtool:platform -->
placeholder
<!-- END bbtool:platform -->

## Use in your project

<!-- BEGIN bbtool:wiring -->
placeholder
<!-- END bbtool:wiring -->

## Links

<!-- BEGIN bbtool:links -->
placeholder
<!-- END bbtool:links -->
"""

_BRIEF_HEADER = "#pragma once\n/** @brief Fake component for tests. */\nvoid bb_fake_noop(void);\n"


def _make_component(root: str, name: str, cmake_body: str, readme: str = README_TEMPLATE,
                     header: str = None) -> Path:
    comp = Path(root) / "components" / name
    comp.mkdir(parents=True)
    (comp / "CMakeLists.txt").write_text(cmake_body, encoding="utf-8")
    (comp / "README.md").write_text(readme, encoding="utf-8")
    if header is not None:
        include_dir = comp / "include"
        include_dir.mkdir(parents=True, exist_ok=True)
        (include_dir / f"{name}.h").write_text(header, encoding="utf-8")
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
        # Mirrors the deleted bb_info component's former CMakeLists.txt shape:
        # an inline comment block
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
        with tempfile.TemporaryDirectory() as td:
            text = _render_deps(Path(td), "bb_fake", [], [])
            self.assertIn("_(none)_", text)

    def test_render_deps_nonempty(self):
        with tempfile.TemporaryDirectory() as td:
            text = _render_deps(Path(td), "bb_fake", ["bb_core"], ["bb_log"])
            self.assertIn("`bb_core`", text)
            self.assertIn("`bb_log`", text)

    def test_render_deps_merges_requires_and_priv_requires_sorted(self):
        with tempfile.TemporaryDirectory() as td:
            text = _render_deps(Path(td), "bb_fake", ["bb_z"], ["bb_a"])
            self.assertLess(text.index("bb_a"), text.index("bb_z"))

    def test_render_deps_table_header(self):
        with tempfile.TemporaryDirectory() as td:
            text = _render_deps(Path(td), "bb_fake", ["bb_core"], [])
            self.assertIn("| Component | Kind | Role | Docs |", text)


class TestRenderDepsKind(unittest.TestCase):
    def test_requires_only_is_public(self):
        with tempfile.TemporaryDirectory() as td:
            text = _render_deps(Path(td), "bb_fake", ["bb_core"], [])
            self.assertIn("| `bb_core` | public |", text)

    def test_priv_requires_only_is_private(self):
        with tempfile.TemporaryDirectory() as td:
            text = _render_deps(Path(td), "bb_fake", [], ["bb_log"])
            self.assertIn("| `bb_log` | private |", text)

    def test_dep_in_both_requires_and_priv_requires_is_public(self):
        with tempfile.TemporaryDirectory() as td:
            text = _render_deps(Path(td), "bb_fake", ["bb_core"], ["bb_core"])
            self.assertIn("| `bb_core` | public |", text)
            self.assertEqual(text.count("bb_core"), 2)  # one row only (deduped)

    def test_self_reference_dependency_renders_without_error(self):
        # A component listing itself in REQUIRES is malformed CMake but must
        # not crash the generator — it renders as an ordinary (public) row.
        with tempfile.TemporaryDirectory() as td:
            text = _render_deps(Path(td), "bb_fake", ["bb_fake"], [])
            self.assertIn("| `bb_fake` | public |", text)


class TestDepRoleAndLink(unittest.TestCase):
    def test_dep_with_readme_uses_first_sentence_and_own_readme_link(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            dep = root / "components" / "bb_dep"
            dep.mkdir(parents=True)
            (dep / "README.md").write_text(
                "# bb_dep\n\nFirst sentence here. Second sentence ignored.\n",
                encoding="utf-8",
            )
            role, link = _dep_role_and_link(root, "bb_dep")
            self.assertEqual(role, "First sentence here.")
            self.assertEqual(link, "../bb_dep/README.md")

    def test_local_component_dir_with_no_readme_falls_back_to_index(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            (root / "components" / "bb_bare").mkdir(parents=True)
            role, link = _dep_role_and_link(root, "bb_bare")
            self.assertEqual(role, "—")
            self.assertEqual(link, "../README.md")

    def test_external_sdk_dependency_has_no_link(self):
        # No components/<dep>/ directory at all -> external SDK dependency
        # (e.g. esp_timer, freertos): plain text, no link, role "—".
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            role, link = _dep_role_and_link(root, "esp_timer")
            self.assertEqual(role, "—")
            self.assertIsNone(link)

    def test_render_deps_renders_external_dep_as_plain_text(self):
        with tempfile.TemporaryDirectory() as td:
            text = _render_deps(Path(td), "bb_fake", [], ["freertos"])
            self.assertIn("| `freertos` | private | — | freertos |", text)
            self.assertNotIn("[freertos]", text)

    def test_render_deps_renders_local_dep_as_link(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            (root / "components" / "bb_dep").mkdir(parents=True)
            (root / "components" / "bb_dep" / "README.md").write_text(
                "# bb_dep\n\nDep purpose.\n", encoding="utf-8",
            )
            text = _render_deps(root, "bb_fake", ["bb_dep"], [])
            self.assertIn("[bb_dep](../bb_dep/README.md)", text)


class TestExtractFirstSentence(unittest.TestCase):
    def test_skips_title_and_blank_lines(self):
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "README.md"
            path.write_text("# title\n\n\nBrief here. More text.\n", encoding="utf-8")
            self.assertEqual(_extract_first_sentence(path), "Brief here.")

    def test_no_prose_line_returns_em_dash(self):
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "README.md"
            path.write_text("# title\n\n", encoding="utf-8")
            self.assertEqual(_extract_first_sentence(path), "—")

    def test_no_terminal_punctuation_returns_whole_line(self):
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "README.md"
            path.write_text("# title\n\nNo punctuation at all\n", encoding="utf-8")
            self.assertEqual(_extract_first_sentence(path), "No punctuation at all")

    def test_marker_safe_skips_begin_marker_line(self):
        # A dep whose README was already converted to a bbtool:brief marker:
        # the region BODY (the line right after BEGIN) is the correct role
        # to surface, and the marker delimiter lines themselves must not
        # leak into the extracted text.
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "README.md"
            path.write_text(
                "# bb_dep\n\n"
                "<!-- BEGIN bbtool:brief -->\n"
                "Converted brief body sentence. More.\n"
                "<!-- END bbtool:brief -->\n",
                encoding="utf-8",
            )
            self.assertEqual(_extract_first_sentence(path), "Converted brief body sentence.")

    def test_marker_safe_skips_leading_end_marker_with_blank_before_title(self):
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "README.md"
            path.write_text(
                "<!-- BEGIN bbtool:mystery -->\n"
                "# title\n"
                "<!-- END bbtool:mystery -->\n\n"
                "Real prose sentence.\n",
                encoding="utf-8",
            )
            self.assertEqual(_extract_first_sentence(path), "Real prose sentence.")

    def test_abbreviation_period_does_not_truncate_sentence(self):
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "README.md"
            path.write_text(
                "# title\n\nUses e.g. bb_core for timing, plus a small cache.\n",
                encoding="utf-8",
            )
            self.assertEqual(
                _extract_first_sentence(path),
                "Uses e.g. bb_core for timing, plus a small cache.",
            )

    def test_two_sentence_brief_still_truncates_at_first(self):
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "README.md"
            path.write_text("# title\n\nFirst sentence. Second one.\n", encoding="utf-8")
            self.assertEqual(_extract_first_sentence(path), "First sentence.")


class TestRenderApi(unittest.TestCase):
    def test_lists_headers_and_prefix_line(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            include_dir = root / "components" / "bb_fake" / "include"
            include_dir.mkdir(parents=True)
            (include_dir / "bb_fake.h").write_text("", encoding="utf-8")
            (include_dir / "bb_fake_extra.h").write_text("", encoding="utf-8")
            text = _render_api(root, "bb_fake")
            self.assertIn("- [`bb_fake.h`](include/bb_fake.h)", text)
            self.assertIn("- [`bb_fake_extra.h`](include/bb_fake_extra.h)", text)
            self.assertIn("Public symbols use the `bb_` prefix.", text)
            # sorted
            self.assertLess(text.index("bb_fake.h"), text.index("bb_fake_extra.h"))

    def test_no_include_dir_still_renders_prefix_line(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            text = _render_api(root, "bb_fake")
            self.assertEqual(text.strip(), "Public symbols use the `bb_` prefix.")


class TestRenderBrief(unittest.TestCase):
    def test_sources_from_header_brief(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            comp_dir = root / "components" / "bb_fake"
            include_dir = comp_dir / "include"
            include_dir.mkdir(parents=True)
            # A CMakeLists.txt marks this dir as a leaf component under
            # discovery.py's leaf rule (B1-1084 consumer migration).
            (comp_dir / "CMakeLists.txt").write_text("", encoding="utf-8")
            (include_dir / "bb_fake.h").write_text(_BRIEF_HEADER, encoding="utf-8")
            self.assertEqual(_render_brief(root, "bb_fake"), "Fake component for tests.")

    def test_raises_when_header_has_no_brief(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            comp_dir = root / "components" / "bb_fake"
            include_dir = comp_dir / "include"
            include_dir.mkdir(parents=True)
            (comp_dir / "CMakeLists.txt").write_text("", encoding="utf-8")
            (include_dir / "bb_fake.h").write_text("#pragma once\nvoid a(void);\n", encoding="utf-8")
            with self.assertRaises(DocsGenError):
                _render_brief(root, "bb_fake")

    def test_raises_when_header_missing(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            with self.assertRaises(DocsGenError):
                _render_brief(root, "bb_fake")

    def test_raises_when_component_not_discovered(self):
        """`name` has no `components/<name>/` entry at all (no
        CMakeLists.txt anywhere on that branch) — `primary_header` returns
        `None`, and `_render_brief` must raise DocsGenError naming the real
        cause ('not a discovered component'), not crash on a None header or
        silently point at a fabricated path."""
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            (root / "components" / "bb_fake" / "include").mkdir(parents=True)
            with self.assertRaises(DocsGenError) as ctx:
                _render_brief(root, "bb_fake")
            self.assertIn("not a discovered component", str(ctx.exception))


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


class TestSelfWikiLink(unittest.TestCase):
    def test_derives_components_subdir_link(self):
        self.assertEqual(
            _self_wiki_link("https://example.test/wiki", "bb_foo"),
            "https://example.test/wiki/components/bb_foo",
        )

    def test_strips_trailing_slash(self):
        self.assertEqual(
            _self_wiki_link("https://example.test/wiki/", "bb_foo"),
            "https://example.test/wiki/components/bb_foo",
        )

    def test_empty_wiki_base_returns_none(self):
        self.assertIsNone(_self_wiki_link("", "bb_foo"))


class TestDedupeLinks(unittest.TestCase):
    def test_preserves_order_first_occurrence_wins(self):
        merged = _dedupe_links(["a", "b"], ["b", "c"], ["a", "d"])
        self.assertEqual(merged, ["a", "b", "c", "d"])

    def test_skips_falsy_entries(self):
        merged = _dedupe_links([], ["a"], [])
        self.assertEqual(merged, ["a"])


class TestRenderLinks(unittest.TestCase):
    def test_no_docs_config_renders_placeholder(self):
        text = _render_links({}, "bb_foo")
        self.assertEqual(text, "_(no links configured)_")

    def test_merges_self_global_and_component_links(self):
        config = {
            "docs": {
                "repo_url": "https://example.test/repo",
                "wiki_base": "https://example.test/wiki",
                "links": ["https://example.test/wiki/Component-Docs"],
                "component_links": {"bb_foo": ["https://example.test/wiki/foo-notes"]},
            }
        }
        text = _render_links(config, "bb_foo")
        self.assertIn("- Repository: [https://example.test/repo](https://example.test/repo)", text)
        self.assertIn("https://example.test/wiki/components/bb_foo", text)
        self.assertIn("https://example.test/wiki/Component-Docs", text)
        self.assertIn("https://example.test/wiki/foo-notes", text)

    def test_other_component_does_not_get_unrelated_component_links(self):
        config = {
            "docs": {"component_links": {"bb_foo": ["https://example.test/wiki/foo-notes"]}}
        }
        text = _render_links(config, "bb_bar")
        self.assertNotIn("foo-notes", text)


class TestRenderWiring(unittest.TestCase):
    def test_renders_absolute_wiki_base_url(self):
        config = {"docs": {"wiki_base": "https://example.test/wiki"}}
        text = _render_wiring(config, "bb_foo")
        self.assertIn("https://example.test/wiki/components/bb_foo#use", text)
        self.assertIn("wiring guide", text)
        self.assertNotIn("../../wiki", text)

    def test_strips_trailing_slash_on_wiki_base(self):
        config = {"docs": {"wiki_base": "https://example.test/wiki/"}}
        text = _render_wiring(config, "bb_foo")
        self.assertIn("https://example.test/wiki/components/bb_foo#use", text)

    def test_degrades_gracefully_when_wiki_base_unset(self):
        text = _render_wiring({}, "bb_foo")
        self.assertNotIn("[wiring guide]", text)  # no markdown link syntax
        self.assertNotIn("http", text)
        self.assertIn("wiring guide", text)


def _write_metrics_baseline(root: Path, target: str, components: dict, heap: dict = None) -> Path:
    metrics_dir = root / ".baseline" / "bbtool" / "metrics"
    metrics_dir.mkdir(parents=True, exist_ok=True)
    payload = {
        "target": target, "arch": "xtensa",
        "config": {"label": "default", "toolchain": "esp-idf", "sdkconfig_sha": "x", "snapshot": "x"},
        "flash": {"text": 1, "data": 1, "bss": 1, "flash_total": 2, "components": components},
        "heap": heap or {"min_free": None, "high_water": None, "regions": None, "source": None},
    }
    path = metrics_dir / f"{target}.json"
    path.write_text(json.dumps(payload), encoding="utf-8")
    return path


class TestRenderBudget(unittest.TestCase):
    def test_no_baseline_dir_renders_fail_soft_placeholder(self):
        with tempfile.TemporaryDirectory() as td:
            text = _render_budget(Path(td), "bb_example")
            self.assertEqual(text, "_(no baseline)_")

    def test_component_absent_from_baseline_renders_fail_soft_placeholder(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write_metrics_baseline(root, "esp32", {"bb_other": 100})
            text = _render_budget(root, "bb_example")
            self.assertEqual(text, "_(no baseline)_")

    def test_malformed_baseline_file_skipped_not_raised(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            metrics_dir = root / ".baseline" / "bbtool" / "metrics"
            metrics_dir.mkdir(parents=True)
            (metrics_dir / "esp32.json").write_text("not json{{{", encoding="utf-8")
            text = _render_budget(root, "bb_example")
            self.assertEqual(text, "_(no baseline)_")

    def test_flash_only_renders_table_without_heap_columns(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write_metrics_baseline(root, "esp32", {"bb_example": 4096})
            text = _render_budget(root, "bb_example")
            self.assertIn("| Target | flash | Δ vs baseline |", text)
            self.assertNotIn("min_free", text)
            self.assertIn("| `esp32` | 4096 | — |", text)

    def test_heap_populated_renders_heap_columns(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write_metrics_baseline(
                root, "esp32", {"bb_example": 4096},
                heap={"min_free": 12345, "high_water": 20000, "regions": ["dram"], "source": "device"},
            )
            text = _render_budget(root, "bb_example")
            self.assertIn("min_free", text)
            self.assertIn("high_water", text)
            self.assertIn("12345", text)
            self.assertIn("20000", text)

    def test_multiple_targets_sorted_by_target_name(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write_metrics_baseline(root, "esp32c3", {"bb_example": 100})
            _write_metrics_baseline(root, "esp32", {"bb_example": 200})
            text = _render_budget(root, "bb_example")
            idx_esp32 = text.index("`esp32`")
            idx_esp32c3 = text.index("`esp32c3`")
            self.assertLess(idx_esp32, idx_esp32c3)


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

    def test_readme_with_no_markers_is_a_noop(self):
        # A pre-template README (hand-authored, no bbtool markers at all)
        # must be left byte-for-byte untouched by `docs gen`.
        with tempfile.TemporaryDirectory() as td:
            comp = _make_component(
                td, "bb_fake",
                "idf_component_register(REQUIRES bb_core)\n",
                readme="# bb_fake\n\nHand-authored, no markers at all.\n",
            )
            before = (comp / "README.md").read_text(encoding="utf-8")
            results = gen_all(td)
            self.assertEqual(len(results), 1)
            _, changed = results[0]
            self.assertFalse(changed)
            self.assertEqual((comp / "README.md").read_text(encoding="utf-8"), before)

    def test_full_template_brief_and_api_regions_idempotent(self):
        with tempfile.TemporaryDirectory() as td:
            comp = _make_component(
                td, "bb_fake",
                "idf_component_register(REQUIRES bb_core)\n",
                readme=FULL_TEMPLATE,
                header=_BRIEF_HEADER,
            )
            results = gen_all(td)
            self.assertEqual(len(results), 1)
            _, changed = results[0]
            self.assertTrue(changed)
            content = (comp / "README.md").read_text(encoding="utf-8")
            self.assertIn("Fake component for tests.", content)
            self.assertIn("- [`bb_fake.h`](include/bb_fake.h)", content)
            self.assertIn("Public symbols use the `bb_` prefix.", content)

            snapshot = content
            results2 = gen_all(td)
            _, changed2 = results2[0]
            self.assertFalse(changed2, "second gen run must report no change")
            self.assertEqual((comp / "README.md").read_text(encoding="utf-8"), snapshot)

    def test_budget_region_idempotent_with_and_without_baseline(self):
        budget_template = (
            "# bb_fake\n\n"
            "## Footprint\n\n"
            "<!-- BEGIN bbtool:budget -->\nplaceholder\n<!-- END bbtool:budget -->\n"
        )
        with tempfile.TemporaryDirectory() as td:
            comp = _make_component(
                td, "bb_fake",
                "idf_component_register(REQUIRES bb_core)\n",
                readme=budget_template,
            )
            # No baseline yet -> fail-soft placeholder, still idempotent.
            results = gen_all(td)
            self.assertEqual(len(results), 1)
            content = (comp / "README.md").read_text(encoding="utf-8")
            self.assertIn("_(no baseline)_", content)
            results2 = gen_all(td)
            _, changed2 = results2[0]
            self.assertFalse(changed2)

            # Baseline appears -> region updates, then stabilizes again.
            _write_metrics_baseline(Path(td), "esp32", {"bb_fake": 512})
            results3 = gen_all(td)
            _, changed3 = results3[0]
            self.assertTrue(changed3)
            content3 = (comp / "README.md").read_text(encoding="utf-8")
            self.assertIn("| `esp32` | 512 | — |", content3)
            snapshot = content3
            results4 = gen_all(td)
            _, changed4 = results4[0]
            self.assertFalse(changed4, "second gen run after baseline appears must be a no-op")
            self.assertEqual((comp / "README.md").read_text(encoding="utf-8"), snapshot)

    def test_brief_marker_without_header_brief_raises(self):
        with tempfile.TemporaryDirectory() as td:
            _make_component(
                td, "bb_fake",
                "idf_component_register(REQUIRES bb_core)\n",
                readme=FULL_TEMPLATE,
                header="#pragma once\nvoid bb_fake_noop(void);\n",  # no @brief
            )
            with self.assertRaises(DocsGenError):
                gen_all(td)

    def test_dir_not_a_component_no_readme_no_cmake(self):
        # A stray directory under components/ with neither README nor
        # CMakeLists is simply skipped (not a component at all).
        with tempfile.TemporaryDirectory() as td:
            (Path(td) / "components" / "not_a_component").mkdir(parents=True)
            self.assertEqual(gen_all(td), [])


class TestCmdGen(unittest.TestCase):
    def test_brief_marker_without_header_brief_returns_1_no_traceback(self):
        with tempfile.TemporaryDirectory() as td:
            _make_component(
                td, "bb_fake",
                "idf_component_register(REQUIRES bb_core)\n",
                readme=FULL_TEMPLATE,
                header="#pragma once\nvoid bb_fake_noop(void);\n",  # no @brief
            )
            stderr = io.StringIO()
            with redirect_stderr(stderr):
                rc = _cmd_gen(td)
            self.assertEqual(rc, 1)
            self.assertNotIn("Traceback", stderr.getvalue())
            self.assertIn("bbtool docs gen: error:", stderr.getvalue())


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

    def test_fires_on_brief_marker_without_header_brief(self):
        with tempfile.TemporaryDirectory() as td:
            _make_component(
                td, "bb_fake", "idf_component_register()\n",
                readme=FULL_TEMPLATE,
                header="#pragma once\nvoid bb_fake_noop(void);\n",  # no @brief
            )
            ctx = Context(root=td, config={})
            violations = _check_component_readme(ctx)
            self.assertTrue(violations)
            self.assertIn("bb_fake", violations[0]["path"])
            self.assertIn("@brief", violations[0]["detail"])

    def test_passes_when_brief_marker_and_header_brief_both_present(self):
        with tempfile.TemporaryDirectory() as td:
            _make_component(
                td, "bb_fake", "idf_component_register()\n",
                readme=FULL_TEMPLATE,
                header=_BRIEF_HEADER,
            )
            ctx = Context(root=td, config={})
            self.assertEqual(_check_component_readme(ctx), [])

    def test_no_brief_marker_is_untouched_by_the_check(self):
        with tempfile.TemporaryDirectory() as td:
            _make_component(td, "bb_documented", "idf_component_register()\n")  # README_TEMPLATE, no brief marker
            ctx = Context(root=td, config={})
            self.assertEqual(_check_component_readme(ctx), [])


_DOCS_CONFIG = {
    "docs": {
        "repo_url": "https://github.com/example-org/example-repo",
        "wiki_base": "https://github.com/example-org/example-repo/wiki",
        "links": ["https://github.com/example-org/example-repo/wiki/Component-Docs"],
        "component_links": {
            "bb_widget": ["https://github.com/example-org/example-repo/wiki/widget-notes"],
        },
    }
}


class TestComponentPrefix(unittest.TestCase):
    def test_prefix_before_first_underscore(self):
        self.assertEqual(_component_prefix("bb_foo"), "bb")
        self.assertEqual(_component_prefix("bb_foo_bar"), "bb")

    def test_prefix_no_underscore(self):
        self.assertEqual(_component_prefix("standalone"), "standalone")

    def test_prefix_leading_underscore(self):
        self.assertEqual(_component_prefix("_bb_widget"), "bb")


def _scaffold_with_brief(root, component, config):
    """scaffold_component, then seed the primary header with an @brief so
    the immediate post-scaffold `_gen_component_readme` brief region
    resolves instead of raising DocsGenError."""
    comp_dir = Path(root) / "components" / component
    include_dir = comp_dir / "include"
    include_dir.mkdir(parents=True, exist_ok=True)
    # A CMakeLists.txt marks this dir as a leaf component under
    # discovery.py's leaf rule (B1-1084 consumer migration) — without it,
    # header_annot.primary_header() can't resolve this fixture's header at
    # all and _render_brief raises DocsGenError before the @brief seeded
    # below is ever read.
    (comp_dir / "CMakeLists.txt").write_text("", encoding="utf-8")
    (include_dir / f"{component}.h").write_text(
        f"#pragma once\n/** @brief {component} test fixture. */\nvoid noop(void);\n",
        encoding="utf-8",
    )
    return scaffold_component(root, component, config)


class TestDocsScaffold(unittest.TestCase):
    def test_scaffold_stamps_expected_content(self):
        with tempfile.TemporaryDirectory() as td:
            path = _scaffold_with_brief(td, "bb_widget", {})
            self.assertTrue(path.exists())
            content = path.read_text(encoding="utf-8")
            self.assertIn("# bb_widget", content)
            self.assertIn("bb_widget test fixture.", content)
            self.assertIn("include/bb_widget.h", content)
            self.assertIn("`bb_` prefix", content)
            self.assertIn("<!-- BEGIN bbtool:brief -->", content)
            self.assertIn("<!-- BEGIN bbtool:api -->", content)
            self.assertIn("<!-- BEGIN bbtool:deps -->", content)
            self.assertIn("<!-- BEGIN bbtool:platform -->", content)
            self.assertIn("<!-- BEGIN bbtool:links -->", content)
            self.assertIn("<!-- BEGIN bbtool:wiring -->", content)
            # Links section is unconditional now (marker-generated); docs
            # config absent -> placeholder text, not an omitted section.
            self.assertIn("## Links", content)
            self.assertIn("_(no links configured)_", content)
            self.assertIn("wiring guide", content)
            self.assertIn(
                "_Generated by `bbtool docs` — see [doc conventions]"
                "(../../wiki/Component-Docs)._",
                content,
            )
            self.assertEqual(find_dangling_tokens(content), [])

    def test_scaffold_refuses_to_overwrite(self):
        with tempfile.TemporaryDirectory() as td:
            path = _scaffold_with_brief(td, "bb_widget", {})
            original = path.read_text(encoding="utf-8")
            with self.assertRaises(FileExistsError):
                scaffold_component(td, "bb_widget", {})
            # File must be untouched by the refused attempt.
            self.assertEqual(path.read_text(encoding="utf-8"), original)

    def test_scaffold_without_header_brief_raises(self):
        with tempfile.TemporaryDirectory() as td:
            with self.assertRaises(DocsGenError):
                scaffold_component(td, "bb_widget", {})

    def test_scaffold_with_docs_config_renders_links(self):
        with tempfile.TemporaryDirectory() as td:
            path = _scaffold_with_brief(td, "bb_widget", _DOCS_CONFIG)
            content = path.read_text(encoding="utf-8")
            self.assertIn("## Links", content)
            self.assertIn("https://github.com/example-org/example-repo", content)
            self.assertIn(
                "https://github.com/example-org/example-repo/wiki/components/bb_widget",
                content,
            )
            self.assertIn(
                "https://github.com/example-org/example-repo/wiki/Component-Docs", content
            )
            self.assertIn("https://github.com/example-org/example-repo/wiki/widget-notes", content)
            self.assertEqual(find_dangling_tokens(content), [])

    def test_scaffold_without_docs_config_renders_placeholder(self):
        with tempfile.TemporaryDirectory() as td:
            path = _scaffold_with_brief(td, "bb_widget", {})
            content = path.read_text(encoding="utf-8")
            self.assertNotIn("{{", content)
            self.assertNotIn("}}", content)
            self.assertIn("## Links", content)
            self.assertIn("_(no links configured)_", content)

    def test_scaffold_deterministic(self):
        with tempfile.TemporaryDirectory() as td1, tempfile.TemporaryDirectory() as td2:
            path1 = _scaffold_with_brief(td1, "bb_widget", _DOCS_CONFIG)
            path2 = _scaffold_with_brief(td2, "bb_widget", _DOCS_CONFIG)
            self.assertEqual(
                path1.read_text(encoding="utf-8"),
                path2.read_text(encoding="utf-8"),
            )

    def test_scaffold_partial_docs_config_missing_wiki_base(self):
        with tempfile.TemporaryDirectory() as td:
            config = {"docs": {"repo_url": "https://github.com/example-org/example-repo"}}
            path = _scaffold_with_brief(td, "bb_widget", config)
            content = path.read_text(encoding="utf-8")
            self.assertNotIn("[]()", content)
            # No wiki_base -> no self wiki link in the generated Links region
            # (the "Use in your project" wiring pointer degrades to plain
            # text when wiki_base is unset — see TestRenderWiring).
            self.assertNotIn(
                "https://github.com/example-org/example-repo/wiki/components/bb_widget",
                content,
            )
            self.assertIn(
                "- Repository: [https://github.com/example-org/example-repo]"
                "(https://github.com/example-org/example-repo)",
                content,
            )

    def test_scaffold_partial_docs_config_missing_repo_url(self):
        with tempfile.TemporaryDirectory() as td:
            config = {"docs": {"wiki_base": "https://github.com/example-org/example-repo/wiki"}}
            path = _scaffold_with_brief(td, "bb_widget", config)
            content = path.read_text(encoding="utf-8")
            self.assertNotIn("[]()", content)
            self.assertNotIn("- Repository:", content)
            self.assertIn(
                "https://github.com/example-org/example-repo/wiki/components/bb_widget",
                content,
            )

    def test_scaffold_empty_docs_config_dict_matches_absent(self):
        with tempfile.TemporaryDirectory() as td1, tempfile.TemporaryDirectory() as td2:
            path1 = _scaffold_with_brief(td1, "bb_widget", {})
            path2 = _scaffold_with_brief(td2, "bb_widget", {"docs": {}})
            self.assertEqual(
                path1.read_text(encoding="utf-8"),
                path2.read_text(encoding="utf-8"),
            )

    def test_scaffolded_readme_is_gen_idempotent(self):
        with tempfile.TemporaryDirectory() as td:
            _scaffold_with_brief(td, "bb_widget", _DOCS_CONFIG)
            comp = Path(td) / "components" / "bb_widget"
            before = (comp / "README.md").read_text(encoding="utf-8")
            results = gen_all(td, _DOCS_CONFIG)
            self.assertEqual(len(results), 1)
            _, changed = results[0]
            self.assertFalse(changed, "docs gen must not churn a freshly-scaffolded README")
            after = (comp / "README.md").read_text(encoding="utf-8")
            self.assertEqual(before, after)


if __name__ == "__main__":
    unittest.main()
