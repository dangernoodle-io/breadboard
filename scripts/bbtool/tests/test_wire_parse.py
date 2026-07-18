"""wire_parse tests: `// bbtool:init` grep-marker parsing over synthetic
header text (decision #735)."""
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from wire_parse import InitEntry, ParseError, ProvidesEntry, parse_markers, parse_provides_markers


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


class TestConsumes(unittest.TestCase):
    def test_consumes_parses_onto_init_entry(self):
        text = "// bbtool:init tier=early fn=bb_example_set_emit consumes=demo_sink\n"
        e = parse_markers(text)[0]
        self.assertEqual(e.consumes, "demo_sink")

    def test_no_consumes_defaults_to_none(self):
        text = "// bbtool:init tier=early fn=bb_x_init\n"
        e = parse_markers(text)[0]
        self.assertIsNone(e.consumes)

    def test_consumes_and_server_together_is_error(self):
        with self.assertRaises(ParseError):
            parse_markers(
                "// bbtool:init tier=regular fn=bb_x_set consumes=demo_sink server=true\n"
            )

    def test_consumes_with_comma_is_error(self):
        """Grammar supports only a single consumes key — unlike provides=/
        requires=, it is not a csv list. A comma is a likely typo and must
        raise, not silently soft-skip as a literal 'a,b' key that never
        matches."""
        with self.assertRaises(ParseError):
            parse_markers("// bbtool:init tier=early fn=bb_x_set consumes=a,b\n")

    def test_ctx_with_consumes_parses(self):
        text = "// bbtool:init tier=early fn=bb_x_set consumes=demo_sink ctx=&s_binding\n"
        e = parse_markers(text)[0]
        self.assertEqual(e.consumes, "demo_sink")
        self.assertEqual(e.ctx, "&s_binding")

    def test_no_ctx_defaults_to_none(self):
        text = "// bbtool:init tier=early fn=bb_x_set consumes=demo_sink\n"
        e = parse_markers(text)[0]
        self.assertIsNone(e.ctx)

    def test_ctx_without_consumes_is_error(self):
        """ctx= is meaningless without a consumes= setter to pass it to."""
        with self.assertRaises(ParseError):
            parse_markers("// bbtool:init tier=early fn=bb_x_init ctx=&s_binding\n")


class TestProvidesMarker(unittest.TestCase):
    def test_minimal_provides_marker(self):
        text = "// bbtool:provides key=demo_sink symbol=bb_example_emit\n"
        entries = parse_provides_markers(text, src_file="bb_example.h")
        self.assertEqual(len(entries), 1)
        e = entries[0]
        self.assertIsInstance(e, ProvidesEntry)
        self.assertEqual(e.key, "demo_sink")
        self.assertEqual(e.symbol, "bb_example_emit")
        self.assertEqual(e.src_file, "bb_example.h")
        self.assertEqual(e.src_line, 1)

    def test_provides_marker_key_order_is_free(self):
        text = "// bbtool:provides symbol=bb_example_emit key=demo_sink\n"
        e = parse_provides_markers(text)[0]
        self.assertEqual(e.key, "demo_sink")
        self.assertEqual(e.symbol, "bb_example_emit")

    def test_provides_marker_never_seen_by_init_parser(self):
        text = "// bbtool:provides key=demo_sink symbol=bb_example_emit\n"
        self.assertEqual(parse_markers(text), [])

    def test_init_marker_never_seen_by_provides_parser(self):
        text = "// bbtool:init tier=early fn=bb_x_init\n"
        self.assertEqual(parse_provides_markers(text), [])

    def test_missing_key_is_error(self):
        with self.assertRaises(ParseError):
            parse_provides_markers("// bbtool:provides symbol=bb_example_emit\n")

    def test_missing_symbol_is_error(self):
        with self.assertRaises(ParseError):
            parse_provides_markers("// bbtool:provides key=demo_sink\n")

    def test_unknown_token_is_error(self):
        with self.assertRaises(ParseError):
            parse_provides_markers(
                "// bbtool:provides key=demo_sink symbol=bb_example_emit bogus=1\n"
            )

    def test_malformed_token_without_equals_is_error(self):
        with self.assertRaises(ParseError):
            parse_provides_markers("// bbtool:provides key=demo_sink garbage\n")

    def test_duplicate_key_is_error(self):
        with self.assertRaises(ParseError):
            parse_provides_markers(
                "// bbtool:provides key=demo_sink key=other symbol=bb_example_emit\n"
            )

    def test_empty_marker_is_error(self):
        with self.assertRaises(ParseError):
            parse_provides_markers("// bbtool:provides\n")


if __name__ == "__main__":
    unittest.main()
