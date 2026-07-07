"""wire_parse tests: `// bbtool:init` grep-marker parsing over synthetic
header text (decision #735)."""
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from wire_parse import InitEntry, ParseError, parse_markers


class TestSingleMarker(unittest.TestCase):
    def test_minimal_marker(self):
        text = "// bbtool:init tier=early fn=bb_x_init\n"
        entries = parse_markers(text, src_file="bb_x.h")
        self.assertEqual(len(entries), 1)
        e = entries[0]
        self.assertEqual(e.tier, "early")
        self.assertEqual(e.fn, "bb_x_init")
        self.assertIsNone(e.order)
        self.assertFalse(e.server)
        self.assertEqual(e.provides, ())
        self.assertEqual(e.requires, ())
        self.assertEqual(e.src_file, "bb_x.h")
        self.assertEqual(e.src_line, 1)

    def test_full_marker(self):
        text = (
            "// bbtool:init tier=regular order=5 fn=bb_x_init server=true "
            "provides=x,y requires=a,b\n"
        )
        e = parse_markers(text)[0]
        self.assertEqual(e.tier, "regular")
        self.assertEqual(e.order, 5)
        self.assertEqual(e.fn, "bb_x_init")
        self.assertTrue(e.server)
        self.assertEqual(e.provides, ("x", "y"))
        self.assertEqual(e.requires, ("a", "b"))

    def test_marker_key_order_is_free(self):
        text = "// bbtool:init fn=bb_x_init tier=pre_http\n"
        e = parse_markers(text)[0]
        self.assertEqual(e.tier, "pre_http")
        self.assertEqual(e.fn, "bb_x_init")

    def test_indented_marker_line(self):
        text = "    // bbtool:init tier=early fn=bb_x_init\n"
        entries = parse_markers(text)
        self.assertEqual(len(entries), 1)


class TestMultipleMarkers(unittest.TestCase):
    def test_multiple_markers_in_order_with_line_numbers(self):
        text = (
            "#pragma once\n"
            "// bbtool:init tier=early fn=bb_a_init\n"
            "bb_err_t bb_a_init(void);\n"
            "\n"
            "// bbtool:init tier=regular fn=bb_b_init\n"
            "bb_err_t bb_b_init(void);\n"
        )
        entries = parse_markers(text, src_file="bb.h")
        self.assertEqual([e.fn for e in entries], ["bb_a_init", "bb_b_init"])
        self.assertEqual(entries[0].src_line, 2)
        self.assertEqual(entries[1].src_line, 5)

    def test_no_markers_returns_empty_list(self):
        self.assertEqual(parse_markers("#pragma once\nint x;\n"), [])


class TestMalformed(unittest.TestCase):
    def test_missing_tier_is_error(self):
        with self.assertRaises(ParseError):
            parse_markers("// bbtool:init fn=bb_x_init\n")

    def test_missing_fn_is_error(self):
        with self.assertRaises(ParseError):
            parse_markers("// bbtool:init tier=early\n")

    def test_unknown_tier_is_error(self):
        with self.assertRaises(ParseError):
            parse_markers("// bbtool:init tier=late fn=bb_x_init\n")

    def test_unknown_key_is_error(self):
        with self.assertRaises(ParseError):
            parse_markers("// bbtool:init tier=early fn=bb_x_init bogus=1\n")

    def test_malformed_token_without_equals_is_error(self):
        with self.assertRaises(ParseError):
            parse_markers("// bbtool:init tier=early fn=bb_x_init garbage\n")

    def test_non_integer_order_is_error(self):
        with self.assertRaises(ParseError):
            parse_markers("// bbtool:init tier=early fn=bb_x_init order=abc\n")

    def test_server_must_be_true(self):
        with self.assertRaises(ParseError):
            parse_markers("// bbtool:init tier=early fn=bb_x_init server=yes\n")

    def test_empty_marker_is_error(self):
        with self.assertRaises(ParseError):
            parse_markers("// bbtool:init\n")

    def test_duplicate_key_is_error(self):
        with self.assertRaises(ParseError):
            parse_markers("// bbtool:init tier=early tier=regular fn=bb_x_init\n")

    def test_error_message_includes_file_and_line(self):
        try:
            parse_markers("\n\n// bbtool:init fn=bb_x_init\n", src_file="bb_x.h")
            self.fail("expected ParseError")
        except ParseError as e:
            self.assertIn("bb_x.h:3", str(e))


if __name__ == "__main__":
    unittest.main()
