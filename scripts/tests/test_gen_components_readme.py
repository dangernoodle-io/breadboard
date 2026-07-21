"""Tests for scripts/gen_components_readme.py's `extract_purpose` /
`main` handling of `header_annot.primary_header`'s `Optional[Path]`
contract (B1-1084 consumer migration).

Regression coverage for a real reachable crash: `primary_header` now
returns `None` for a `components/<name>/` directory with no
`CMakeLists.txt` anywhere on that branch (a group/intermediate dir under
discovery.py's leaf rule, or simply an undiscovered name). Before this
file existed, `extract_purpose` fed that `None` straight into
`extract_brief`, which unconditionally calls `path.is_file()` and raised
`AttributeError: 'NoneType' object has no attribute 'is_file'` -- an
unhandled traceback from a script whose entire job (wired into `make
check` as `docs-index-check`) is a clean CI drift check.
"""
import io
import os
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

import gen_components_readme as gcr  # noqa: E402
from discovery import build_index  # noqa: E402


def _mk_component(root: Path, name: str, header_body: str = None, readme_body: str = None) -> Path:
    """Write a REAL leaf component: components/<name>/CMakeLists.txt (marks
    it discoverable under discovery.py's leaf rule) + optional
    include/<name>.h + optional README.md."""
    comp_dir = root / "components" / name
    comp_dir.mkdir(parents=True, exist_ok=True)
    (comp_dir / "CMakeLists.txt").write_text("", encoding="utf-8")
    if header_body is not None:
        inc = comp_dir / "include"
        inc.mkdir(exist_ok=True)
        (inc / f"{name}.h").write_text(header_body, encoding="utf-8")
    if readme_body is not None:
        (comp_dir / "README.md").write_text(readme_body, encoding="utf-8")
    return comp_dir


def _mk_bare_dir(root: Path, name: str, readme_body: str = None) -> Path:
    """A components/<name>/ dir with NO CMakeLists.txt anywhere under it —
    not a discovered component per discovery.py's leaf rule, but still
    findable by collect_components()'s own depth-1 iterdir() walk."""
    comp_dir = root / "components" / name
    comp_dir.mkdir(parents=True, exist_ok=True)
    if readme_body is not None:
        (comp_dir / "README.md").write_text(readme_body, encoding="utf-8")
    return comp_dir


def _mk_group_component(root: Path, group: str, name: str, header_body: str = None,
                         readme_body: str = None) -> Path:
    """Write a REAL leaf component nested under a group directory:
    components/<group>/<name>/CMakeLists.txt (+ optional header/README)."""
    comp_dir = root / "components" / group / name
    comp_dir.mkdir(parents=True, exist_ok=True)
    (comp_dir / "CMakeLists.txt").write_text("", encoding="utf-8")
    if header_body is not None:
        inc = comp_dir / "include"
        inc.mkdir(exist_ok=True)
        (inc / f"{name}.h").write_text(header_body, encoding="utf-8")
    if readme_body is not None:
        (comp_dir / "README.md").write_text(readme_body, encoding="utf-8")
    return comp_dir


def _mk_group_readme(root: Path, group: str, desc: str = "Group description.",
                      with_marker: bool = True, index_body: str = "placeholder") -> Path:
    """Write a hand-authored components/<group>/README.md: a prose title +
    description, optionally followed by a bbtool:group-index marker
    region."""
    comp_dir = root / "components" / group
    comp_dir.mkdir(parents=True, exist_ok=True)
    text = f"# {group}\n\n{desc}\n"
    if with_marker:
        text += f"\n<!-- BEGIN bbtool:group-index -->\n{index_body}\n<!-- END bbtool:group-index -->\n"
    readme = comp_dir / "README.md"
    readme.write_text(text, encoding="utf-8")
    return readme


class TestExtractPurpose(unittest.TestCase):
    def setUp(self):
        build_index.cache_clear()
        self.addCleanup(build_index.cache_clear)

    def test_sources_brief_from_header(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(os.path.realpath(td))
            comp_dir = _mk_component(
                root, "bb_widget",
                header_body="#pragma once\n/** @brief Widget component. */\nvoid noop(void);\n",
                readme_body="# bb_widget\n\nFallback prose, must not be used.\n",
            )
            with mock.patch.object(gcr, "REPO_ROOT", root):
                purpose = gcr.extract_purpose(comp_dir / "README.md", "bb_widget")
            self.assertEqual(purpose, "Widget component.")

    def test_falls_back_to_readme_prose_when_header_has_no_brief(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(os.path.realpath(td))
            comp_dir = _mk_component(
                root, "bb_widget",
                header_body="#pragma once\nvoid noop(void);\n",
                readme_body="# bb_widget\n\nFirst prose line.\n",
            )
            with mock.patch.object(gcr, "REPO_ROOT", root):
                purpose = gcr.extract_purpose(comp_dir / "README.md", "bb_widget")
            self.assertEqual(purpose, "First prose line.")

    def test_raises_component_index_error_when_not_discovered(self):
        """The regression case: a components/<name>/ dir with a README.md
        but no CMakeLists.txt anywhere under it -- primary_header() returns
        None, and extract_purpose must fail loud with a named cause rather
        than crash with an AttributeError on extract_brief(None)."""
        with tempfile.TemporaryDirectory() as td:
            root = Path(os.path.realpath(td))
            comp_dir = _mk_bare_dir(root, "bb_group", readme_body="# bb_group\n\nNot a real component.\n")
            with mock.patch.object(gcr, "REPO_ROOT", root):
                with self.assertRaises(gcr.ComponentIndexError) as ctx:
                    gcr.extract_purpose(comp_dir / "README.md", "bb_group")
            self.assertIn("bb_group", str(ctx.exception))
            self.assertIn("not a discovered component", str(ctx.exception))


class TestCollectAndBuildContent(unittest.TestCase):
    def setUp(self):
        build_index.cache_clear()
        self.addCleanup(build_index.cache_clear)

    def _patched(self, root: Path):
        return mock.patch.multiple(
            gcr,
            REPO_ROOT=root,
            COMPONENTS_DIR=root / "components",
            OUTPUT_FILE=root / "components" / "README.md",
        )

    def test_collect_components_includes_real_and_bare_dirs(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(os.path.realpath(td))
            _mk_component(
                root, "bb_widget",
                header_body="#pragma once\n/** @brief Widget component. */\nvoid noop(void);\n",
                readme_body="# bb_widget\n\nignored\n",
            )
            (root / "components" / "bb_no_readme").mkdir(parents=True)
            (root / "components" / "bb_no_readme" / "CMakeLists.txt").write_text("", encoding="utf-8")
            with self._patched(root):
                entries = gcr.collect_components()
            self.assertEqual(
                entries,
                [("bb_no_readme", "—"), ("bb_widget", "Widget component.")],
            )

    def test_build_content_raises_component_index_error_for_bare_dir_with_readme(self):
        """A REAL end-to-end reproduction of the crash this file guards
        against: build_content() -> collect_components() ->
        extract_purpose() on a components/<name>/ dir that has a README.md
        but no CMakeLists.txt -- must raise ComponentIndexError, never an
        unhandled AttributeError."""
        with tempfile.TemporaryDirectory() as td:
            root = Path(os.path.realpath(td))
            _mk_bare_dir(root, "bb_group", readme_body="# bb_group\n\nNot a real component.\n")
            with self._patched(root):
                with self.assertRaises(gcr.ComponentIndexError):
                    gcr.build_content()


class TestMain(unittest.TestCase):
    def setUp(self):
        build_index.cache_clear()
        self.addCleanup(build_index.cache_clear)

    def _patched(self, root: Path):
        return mock.patch.multiple(
            gcr,
            REPO_ROOT=root,
            COMPONENTS_DIR=root / "components",
            OUTPUT_FILE=root / "components" / "README.md",
        )

    def test_main_check_returns_1_with_clean_error_no_traceback(self):
        """docs-index-check (`make check`) must fail LOUD with a clean,
        named diagnostic on a bare/undiscovered directory -- never an
        unhandled traceback (the exact regression this test file exists
        to close)."""
        with tempfile.TemporaryDirectory() as td:
            root = Path(os.path.realpath(td))
            _mk_bare_dir(root, "bb_group", readme_body="# bb_group\n\nNot a real component.\n")
            stdout, stderr = io.StringIO(), io.StringIO()
            with self._patched(root):
                with mock.patch.object(sys, "argv", ["gen_components_readme.py", "--check"]):
                    with redirect_stdout(stdout), redirect_stderr(stderr):
                        rc = gcr.main()
            self.assertEqual(rc, 1)
            self.assertNotIn("Traceback", stderr.getvalue())
            self.assertIn("gen_components_readme: error:", stderr.getvalue())
            self.assertIn("bb_group", stderr.getvalue())

    def test_main_generate_mode_also_fails_loud(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(os.path.realpath(td))
            _mk_bare_dir(root, "bb_group", readme_body="# bb_group\n\nNot a real component.\n")
            stdout, stderr = io.StringIO(), io.StringIO()
            with self._patched(root):
                with mock.patch.object(sys, "argv", ["gen_components_readme.py"]):
                    with redirect_stdout(stdout), redirect_stderr(stderr):
                        rc = gcr.main()
            self.assertEqual(rc, 1)
            self.assertNotIn("Traceback", stderr.getvalue())

    def test_main_check_passes_on_a_clean_discovered_tree(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(os.path.realpath(td))
            _mk_component(
                root, "bb_widget",
                header_body="#pragma once\n/** @brief Widget component. */\nvoid noop(void);\n",
                readme_body="# bb_widget\n\nignored\n",
            )
            stdout, stderr = io.StringIO(), io.StringIO()
            with self._patched(root):
                with mock.patch.object(sys, "argv", ["gen_components_readme.py"]):
                    with redirect_stdout(stdout), redirect_stderr(stderr):
                        rc_gen = gcr.main()
                self.assertEqual(rc_gen, 0)
                with mock.patch.object(sys, "argv", ["gen_components_readme.py", "--check"]):
                    with redirect_stdout(stdout), redirect_stderr(stderr):
                        rc_check = gcr.main()
            self.assertEqual(rc_check, 0)


class TestHierarchicalGroups(unittest.TestCase):
    """B1-1084 PR3: two-level component index — a group directory
    (components/<group>/<name>/) gets its own summary row (sourced from its
    own README's first prose line) in the top-level index, plus its own
    generated per-group index page."""

    def setUp(self):
        build_index.cache_clear()
        self.addCleanup(build_index.cache_clear)

    def _patched(self, root: Path):
        return mock.patch.multiple(
            gcr,
            REPO_ROOT=root,
            COMPONENTS_DIR=root / "components",
            OUTPUT_FILE=root / "components" / "README.md",
        )

    def test_group_row_appears_in_top_level_index(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(os.path.realpath(td))
            _mk_group_component(
                root, "bb_display", "bb_display_ssd1306",
                header_body="#pragma once\n/** @brief SSD1306 driver. */\nvoid noop(void);\n",
            )
            _mk_group_readme(root, "bb_display", desc="Display backends.")
            with self._patched(root):
                content = gcr.build_content()
            self.assertIn("| [bb_display/](./bb_display/) | Display backends. |", content)

    def test_group_and_ungrouped_rows_sorted_together(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(os.path.realpath(td))
            _mk_component(root, "bb_alpha", readme_body="# bb_alpha\n\nAlpha component.\n")
            _mk_group_component(root, "bb_group_mid", "bb_zulu")
            _mk_group_readme(root, "bb_group_mid", desc="Mid group.")
            _mk_component(root, "bb_omega", readme_body="# bb_omega\n\nOmega component.\n")
            with self._patched(root):
                content = gcr.build_content()
            idx_alpha = content.index("bb_alpha")
            idx_group = content.index("bb_group_mid")
            idx_omega = content.index("bb_omega")
            self.assertLess(idx_alpha, idx_group)
            self.assertLess(idx_group, idx_omega)

    def test_group_with_no_readme_raises_group_readme_error(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(os.path.realpath(td))
            _mk_group_component(root, "bb_undocumented_group", "bb_leaf")
            with self._patched(root):
                with self.assertRaises(gcr.GroupReadmeError) as ctx:
                    gcr.build_content()
            self.assertIn("bb_undocumented_group", str(ctx.exception))
            self.assertIn("no README.md", str(ctx.exception))

    def test_group_readme_missing_marker_raises_group_readme_error(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(os.path.realpath(td))
            comp_dir = _mk_group_component(root, "bb_group_no_marker", "bb_leaf")
            _mk_group_readme(root, "bb_group_no_marker", with_marker=False)
            with self._patched(root):
                with self.assertRaises(gcr.GroupReadmeError) as ctx:
                    gcr.build_group_contents()
            self.assertIn("bbtool:group-index", str(ctx.exception))
            self.assertTrue(comp_dir.is_dir())  # sanity: fixture actually built

    def test_group_index_table_lists_members_with_relative_links(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(os.path.realpath(td))
            _mk_group_component(
                root, "bb_display", "bb_display_ssd1306",
                header_body="#pragma once\n/** @brief SSD1306 driver. */\nvoid noop(void);\n",
                readme_body="# bb_display_ssd1306\n\nignored\n",
            )
            _mk_group_component(
                root, "bb_display", "bb_display_ili9341",
                header_body="#pragma once\n/** @brief ILI9341 driver. */\nvoid noop(void);\n",
                readme_body="# bb_display_ili9341\n\nignored\n",
            )
            _mk_group_readme(root, "bb_display", desc="Display backends.")
            with self._patched(root):
                results = gcr.build_group_contents()
            self.assertEqual(len(results), 1)
            path, content = results[0]
            self.assertEqual(path, root / "components" / "bb_display" / "README.md")
            self.assertIn("| [bb_display_ili9341](./bb_display_ili9341/) | ILI9341 driver. |", content)
            self.assertIn("| [bb_display_ssd1306](./bb_display_ssd1306/) | SSD1306 driver. |", content)
            # hand-authored prose above the marker survives byte-for-byte
            self.assertIn("Display backends.", content)

    def test_group_index_second_run_is_idempotent(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(os.path.realpath(td))
            _mk_group_component(root, "bb_display", "bb_display_ssd1306", readme_body="ignored")
            _mk_group_readme(root, "bb_display", desc="Display backends.")
            with self._patched(root):
                first = dict(gcr.build_group_contents())
                (root / "components" / "bb_display" / "README.md").write_text(
                    first[root / "components" / "bb_display" / "README.md"], encoding="utf-8"
                )
                second = dict(gcr.build_group_contents())
            self.assertEqual(first, second)

    def test_flat_tree_two_level_generator_is_byte_identical_to_flat_generator(self):
        """The key correctness check for the two-level generator: with no
        group directories on disk, it must degrade EXACTLY to the
        pre-hierarchy flat output."""
        with tempfile.TemporaryDirectory() as td:
            root = Path(os.path.realpath(td))
            _mk_component(
                root, "bb_widget",
                header_body="#pragma once\n/** @brief Widget component. */\nvoid noop(void);\n",
                readme_body="# bb_widget\n\nignored\n",
            )
            _mk_component(root, "bb_no_readme")
            with self._patched(root):
                content = gcr.build_content()
                groups = gcr.build_group_contents()
            self.assertEqual(groups, [])
            self.assertIn("| [bb_widget](./bb_widget/) | Widget component. |", content)
            self.assertIn("| [bb_no_readme](./bb_no_readme/) | — |", content)

    def test_main_writes_and_checks_group_readme(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(os.path.realpath(td))
            _mk_group_component(
                root, "bb_display", "bb_display_ssd1306",
                header_body="#pragma once\n/** @brief SSD1306 driver. */\nvoid noop(void);\n",
            )
            _mk_group_readme(root, "bb_display", desc="Display backends.")
            stdout, stderr = io.StringIO(), io.StringIO()
            with self._patched(root):
                with mock.patch.object(sys, "argv", ["gen_components_readme.py"]):
                    with redirect_stdout(stdout), redirect_stderr(stderr):
                        rc_gen = gcr.main()
                self.assertEqual(rc_gen, 0)
                group_readme = root / "components" / "bb_display" / "README.md"
                self.assertIn("bb_display_ssd1306", group_readme.read_text(encoding="utf-8"))

                with mock.patch.object(sys, "argv", ["gen_components_readme.py", "--check"]):
                    with redirect_stdout(stdout), redirect_stderr(stderr):
                        rc_check = gcr.main()
            self.assertEqual(rc_check, 0)

    def test_check_flags_stale_group_index(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(os.path.realpath(td))
            _mk_group_component(
                root, "bb_display", "bb_display_ssd1306",
                header_body="#pragma once\n/** @brief SSD1306 driver. */\nvoid noop(void);\n",
            )
            _mk_group_readme(root, "bb_display", desc="Display backends.",
                              index_body="STALE CONTENT")
            stdout, stderr = io.StringIO(), io.StringIO()
            with self._patched(root):
                with mock.patch.object(sys, "argv", ["gen_components_readme.py", "--check"]):
                    with redirect_stdout(stdout), redirect_stderr(stderr):
                        rc = gcr.main()
            self.assertEqual(rc, 1)
            self.assertIn("is stale", stderr.getvalue())


if __name__ == "__main__":
    unittest.main()
