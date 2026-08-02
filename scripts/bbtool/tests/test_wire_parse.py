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


class TestArgs(unittest.TestCase):
    def test_args_parses_verbatim_including_commas_and_parens(self):
        text = (
            "// bbtool:init tier=regular fn=bb_wifi_prov_autoinit "
            "args=bb_wifi_prov_default_form_get(),1,NULL\n"
        )
        e = parse_markers(text)[0]
        self.assertEqual(e.args, "bb_wifi_prov_default_form_get(),1,NULL")

    def test_no_args_defaults_to_none(self):
        text = "// bbtool:init tier=early fn=bb_x_init\n"
        e = parse_markers(text)[0]
        self.assertIsNone(e.args)

    def test_args_and_server_together_is_error(self):
        with self.assertRaises(ParseError):
            parse_markers(
                "// bbtool:init tier=regular fn=bb_x_init args=1,2 server=true\n"
            )

    def test_args_and_consumes_together_is_error(self):
        with self.assertRaises(ParseError):
            parse_markers(
                "// bbtool:init tier=early fn=bb_x_set args=1,2 consumes=demo_sink\n"
            )

    def test_empty_args_is_error(self):
        """Mirrors every other key's malformed-token handling: an empty
        value after '=' is a hard ParseError, not a silent empty-string
        args= (which would otherwise render a syntactically bogus
        'fn();' -- indistinguishable from the zero-arg convention but for
        the wrong reason)."""
        with self.assertRaises(ParseError):
            parse_markers("// bbtool:init tier=early fn=bb_x_init args=\n")

    def test_args_with_embedded_space_splits_into_malformed_token(self):
        """args= values are documented whitespace-free -- the marker line
        is itself whitespace-tokenized, so a space inside an intended args=
        value splits it into a second, unrelated token ('2)', with no
        '=') -- surfaces as a malformed-token ParseError, never a silently
        truncated args= value."""
        with self.assertRaises(ParseError) as ctx:
            parse_markers(
                "// bbtool:init tier=early fn=bb_x_init args=foo(1, 2)\n"
            )
        self.assertIn("malformed token", str(ctx.exception))


class TestRegistersRoutes(unittest.TestCase):
    """B1-1280 blind-spot closure: 'registers_routes=true' -- an opt-in
    route-registration signal for the http_wildcard_last ordering guard,
    orthogonal to server=true (see wire_parse's module docstring)."""

    def test_registers_routes_true_parses_onto_init_entry(self):
        text = "// bbtool:init tier=regular fn=bb_x_init registers_routes=true\n"
        e = parse_markers(text)[0]
        self.assertTrue(e.registers_routes)

    def test_no_registers_routes_defaults_to_false(self):
        text = "// bbtool:init tier=early fn=bb_x_init\n"
        e = parse_markers(text)[0]
        self.assertFalse(e.registers_routes)

    def test_registers_routes_combines_with_args(self):
        text = (
            "// bbtool:init tier=regular fn=bb_x_register "
            "args=bb_app_http_handle,\"/ping\" registers_routes=true\n"
        )
        e = parse_markers(text)[0]
        self.assertTrue(e.registers_routes)
        self.assertEqual(e.args, 'bb_app_http_handle,"/ping"')

    def test_registers_routes_and_server_together_is_error(self):
        with self.assertRaises(ParseError):
            parse_markers(
                "// bbtool:init tier=regular fn=bb_x_init server=true "
                "registers_routes=true\n"
            )

    def test_registers_routes_not_true_is_error(self):
        with self.assertRaises(ParseError):
            parse_markers(
                "// bbtool:init tier=regular fn=bb_x_init registers_routes=false\n"
            )

    def test_empty_registers_routes_is_error(self):
        with self.assertRaises(ParseError):
            parse_markers("// bbtool:init tier=early fn=bb_x_init registers_routes=\n")


class TestBindsDataField(unittest.TestCase):
    """'binds_data=true' -- an opt-in marker-visible fact (mirrors
    registers_routes=true's parsing posture exactly) letting a self-bind
    call inside a component's .c body be counted by codegen's bb_data
    binding-cap check, since codegen only greps public headers. This PR is
    tooling-only: no real marker carries this flag yet (see wire.py's
    check_binds_data_cap docstring)."""

    def test_binds_data_true_parses_onto_init_entry(self):
        text = "// bbtool:init tier=regular fn=bb_x_init binds_data=true\n"
        e = parse_markers(text)[0]
        self.assertTrue(e.binds_data)

    def test_binds_data_absent_defaults_to_false(self):
        """The absent-marker regression case -- confirms binds_data behaves
        exactly as today (i.e. did not exist) when omitted."""
        text = "// bbtool:init tier=early fn=bb_x_init\n"
        e = parse_markers(text)[0]
        self.assertFalse(e.binds_data)

    def test_binds_data_combines_with_other_fields(self):
        text = (
            "// bbtool:init tier=regular fn=bb_x_init server=true "
            "binds_data=true\n"
        )
        e = parse_markers(text)[0]
        self.assertTrue(e.binds_data)
        self.assertTrue(e.server)

    def test_binds_data_not_true_is_error(self):
        with self.assertRaises(ParseError):
            parse_markers(
                "// bbtool:init tier=regular fn=bb_x_init binds_data=false\n"
            )

    def test_empty_binds_data_is_error(self):
        with self.assertRaises(ParseError):
            parse_markers("// bbtool:init tier=early fn=bb_x_init binds_data=\n")


class TestComponentField(unittest.TestCase):
    """B1-1275: 'component=<name>' parses onto InitEntry -- this module is
    context-agnostic (manifest vs. component-header rejection is decided one
    layer up, in commands.wire) so it accepts the field unconditionally."""

    def test_component_parses_onto_init_entry(self):
        text = "// bbtool:init tier=regular fn=bb_wifi_prov_autoinit component=bb_wifi_prov\n"
        e = parse_markers(text)[0]
        self.assertEqual(e.component, "bb_wifi_prov")

    def test_no_component_defaults_to_none(self):
        text = "// bbtool:init tier=early fn=bb_x_init\n"
        e = parse_markers(text)[0]
        self.assertIsNone(e.component)

    def test_component_combines_with_args(self):
        text = (
            "// bbtool:init tier=regular fn=bb_wifi_prov_autoinit "
            "component=bb_wifi_prov args=bb_wifi_prov_default_form_get(),1,NULL\n"
        )
        e = parse_markers(text)[0]
        self.assertEqual(e.component, "bb_wifi_prov")
        self.assertEqual(e.args, "bb_wifi_prov_default_form_get(),1,NULL")

    def test_empty_component_is_error(self):
        with self.assertRaises(ParseError):
            parse_markers("// bbtool:init tier=early fn=bb_x_init component=\n")


class TestOutField(unittest.TestCase):
    """out=<varname>:<c-type> -- parses onto InitEntry as out_var/out_type,
    same context-agnostic posture as component= (rejection, if any, is
    decided one layer up, in commands.wire)."""

    def test_out_parses_onto_init_entry(self):
        text = "// bbtool:init tier=regular fn=bb_lifecycle_register out=s_binding:bb_lifecycle_t\n"
        e = parse_markers(text)[0]
        self.assertEqual(e.out_var, "s_binding")
        self.assertEqual(e.out_type, "bb_lifecycle_t")

    def test_no_out_defaults_to_none(self):
        text = "// bbtool:init tier=early fn=bb_x_init\n"
        e = parse_markers(text)[0]
        self.assertIsNone(e.out_var)
        self.assertIsNone(e.out_type)

    def test_out_combines_with_args(self):
        text = (
            "// bbtool:init tier=regular fn=bb_lifecycle_register "
            "out=s_binding:bb_lifecycle_t args=cfg,&s_binding\n"
        )
        e = parse_markers(text)[0]
        self.assertEqual(e.out_var, "s_binding")
        self.assertEqual(e.out_type, "bb_lifecycle_t")
        self.assertEqual(e.args, "cfg,&s_binding")

    def test_out_missing_colon_is_error(self):
        with self.assertRaises(ParseError) as ctx:
            parse_markers("// bbtool:init tier=early fn=bb_x_init out=s_binding\n")
        self.assertIn("':'", str(ctx.exception))

    def test_out_empty_varname_is_error(self):
        with self.assertRaises(ParseError):
            parse_markers("// bbtool:init tier=early fn=bb_x_init out=:bb_lifecycle_t\n")

    def test_out_empty_type_is_error(self):
        with self.assertRaises(ParseError):
            parse_markers("// bbtool:init tier=early fn=bb_x_init out=s_binding:\n")

    def test_out_empty_value_is_error(self):
        """Mirrors component='s/args='s empty-value handling: 'out=' alone
        (nothing after '=') is already caught by the generic malformed-token
        check before out='s own ':' validation ever runs."""
        with self.assertRaises(ParseError):
            parse_markers("// bbtool:init tier=early fn=bb_x_init out=\n")

    def test_duplicate_out_key_is_error(self):
        with self.assertRaises(ParseError):
            parse_markers(
                "// bbtool:init tier=early fn=bb_x_init out=a:int out=b:int\n"
            )


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
