"""header_annot tests: @brief extraction across comment styles, missing/absent cases."""
import os
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from header_annot import extract_brief, primary_header


def _write(td: str, content: str) -> Path:
    path = Path(td) / "bb_example.h"
    path.write_text(content, encoding="utf-8")
    return path


class TestExtractBriefBlockComment(unittest.TestCase):
    def test_single_line_block_comment(self):
        with tempfile.TemporaryDirectory() as td:
            path = _write(td, "#pragma once\n/** @brief Single line brief. */\nvoid a(void);\n")
            self.assertEqual(extract_brief(path), "Single line brief.")

    def test_multiline_block_comment_joins_continuations(self):
        with tempfile.TemporaryDirectory() as td:
            content = (
                "#pragma once\n"
                "/**\n"
                " * @brief Foo bar baz.\n"
                " * More text here.\n"
                " */\n"
                "void a(void);\n"
            )
            path = _write(td, content)
            self.assertEqual(extract_brief(path), "Foo bar baz. More text here.")

    def test_block_comment_stops_at_next_tag(self):
        with tempfile.TemporaryDirectory() as td:
            content = (
                "#pragma once\n"
                "/**\n"
                " * @brief Foo bar.\n"
                " * @note Unrelated detail.\n"
                " */\n"
            )
            path = _write(td, content)
            self.assertEqual(extract_brief(path), "Foo bar.")

    def test_block_comment_stops_at_blank_line(self):
        with tempfile.TemporaryDirectory() as td:
            content = (
                "#pragma once\n"
                "/**\n"
                " * @brief Foo bar.\n"
                " *\n"
                " * Second paragraph, not appended.\n"
                " */\n"
            )
            path = _write(td, content)
            self.assertEqual(extract_brief(path), "Foo bar.")


class TestExtractBriefLineComments(unittest.TestCase):
    def test_triple_slash_style(self):
        with tempfile.TemporaryDirectory() as td:
            content = "#pragma once\n/// @brief Line comment brief.\n/// Second line.\nvoid a(void);\n"
            path = _write(td, content)
            self.assertEqual(extract_brief(path), "Line comment brief. Second line.")

    def test_bang_slash_style(self):
        with tempfile.TemporaryDirectory() as td:
            content = "#pragma once\n//! @brief Bang style brief.\nvoid a(void);\n"
            path = _write(td, content)
            self.assertEqual(extract_brief(path), "Bang style brief.")

    def test_line_comment_stops_at_non_comment_line(self):
        with tempfile.TemporaryDirectory() as td:
            content = "#pragma once\n/// @brief First.\nvoid a(void);\n/// not appended\n"
            path = _write(td, content)
            self.assertEqual(extract_brief(path), "First.")

    def test_triple_slash_stops_at_next_tag(self):
        with tempfile.TemporaryDirectory() as td:
            content = (
                "#pragma once\n"
                "/// @brief First sentence.\n"
                "/// @param x foo\n"
                "void a(int x);\n"
            )
            path = _write(td, content)
            self.assertEqual(extract_brief(path), "First sentence.")

    def test_bang_slash_stops_at_next_tag(self):
        with tempfile.TemporaryDirectory() as td:
            content = (
                "#pragma once\n"
                "//! @brief First sentence.\n"
                "//! @return 0 on success\n"
                "int a(void);\n"
            )
            path = _write(td, content)
            self.assertEqual(extract_brief(path), "First sentence.")


class TestExtractBriefAbsentOrMissing(unittest.TestCase):
    def test_no_brief_tag_returns_none(self):
        with tempfile.TemporaryDirectory() as td:
            path = _write(td, "#pragma once\nvoid a(void);\n")
            self.assertIsNone(extract_brief(path))

    def test_missing_file_returns_none(self):
        self.assertIsNone(extract_brief(Path("/nonexistent/dir/bb_missing.h")))

    def test_first_of_multiple_briefs_wins(self):
        with tempfile.TemporaryDirectory() as td:
            content = (
                "#pragma once\n"
                "/** @brief First one. */\n"
                "void a(void);\n"
                "/** @brief Second one. */\n"
                "void b(void);\n"
            )
            path = _write(td, content)
            self.assertEqual(extract_brief(path), "First one.")


class TestPrimaryHeader(unittest.TestCase):
    def test_derives_conventional_path(self):
        root = Path("/repo")
        self.assertEqual(
            primary_header(root, "bb_example"),
            root / "components" / "bb_example" / "include" / "bb_example.h",
        )


if __name__ == "__main__":
    unittest.main()
