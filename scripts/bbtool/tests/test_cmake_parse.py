"""cmake_parse tests: REQUIRES/PRIV_REQUIRES parsing (incl. set()/list(APPEND)
variable indirection) + bbtool-scaffold-hint comment parsing."""
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from cmake_parse import (
    ConditionalSetError,
    parse_hints,
    parse_requires,
    strip_cmake_comments,
)


class TestParseRequiresLiteral(unittest.TestCase):
    def test_requires_and_priv_requires(self):
        cmake = (
            "idf_component_register(\n"
            '    SRCS "src/bb_fake.c"\n'
            "    INCLUDE_DIRS \"include\"\n"
            "    REQUIRES bb_core bb_json\n"
            "    PRIV_REQUIRES bb_log esp_timer\n"
            ")\n"
        )
        requires, priv = parse_requires(cmake)
        self.assertEqual(requires, ["bb_core", "bb_json"])
        self.assertEqual(priv, ["bb_log", "esp_timer"])

    def test_sorted_and_deduped(self):
        cmake = "idf_component_register(REQUIRES bb_json bb_core bb_core)\n"
        requires, priv = parse_requires(cmake)
        self.assertEqual(requires, ["bb_core", "bb_json"])
        self.assertEqual(priv, [])

    def test_no_requires(self):
        cmake = 'idf_component_register(SRCS "a.c" INCLUDE_DIRS "include")\n'
        requires, priv = parse_requires(cmake)
        self.assertEqual((requires, priv), ([], []))

    def test_no_register_call(self):
        self.assertEqual(parse_requires("# nothing here\n"), ([], []))

    def test_comments_do_not_pollute_requires(self):
        cmake = (
            "idf_component_register(\n"
            "    REQUIRES bb_core # bb_json would be wrong here\n"
            ")\n"
        )
        requires, _ = parse_requires(cmake)
        self.assertEqual(requires, ["bb_core"])


class TestParseRequiresVarIndirection(unittest.TestCase):
    """The 6 real components (bb_display, bb_fan, bb_i2c, bb_led, bb_log,
    bb_sink_udp) declare REQUIRES/PRIV_REQUIRES via a `set(VAR ...)` a few
    lines above idf_component_register(...REQUIRES ${VAR}...) instead of
    literal tokens."""

    def test_set_then_requires_var(self):
        cmake = (
            "set(_led_reqs bb_core)\n"
            "set(_led_priv_reqs bb_log)\n"
            "idf_component_register(\n"
            "    INCLUDE_DIRS \"include\"\n"
            "    REQUIRES ${_led_reqs}\n"
            "    PRIV_REQUIRES ${_led_priv_reqs})\n"
        )
        requires, priv = parse_requires(cmake)
        self.assertEqual(requires, ["bb_core"])
        self.assertEqual(priv, ["bb_log"])

    def test_list_append_extends_var(self):
        cmake = (
            "set(BB_FAN_PRIV_REQUIRES bb_log)\n"
            "if(CONFIG_BB_FAN_AUTOFAN)\n"
            "    list(APPEND BB_FAN_PRIV_REQUIRES esp_timer)\n"
            "endif()\n"
            "idf_component_register(\n"
            "    REQUIRES bb_core bb_json\n"
            "    PRIV_REQUIRES ${BB_FAN_PRIV_REQUIRES})\n"
        )
        requires, priv = parse_requires(cmake)
        self.assertEqual(requires, ["bb_core", "bb_json"])
        self.assertEqual(priv, ["bb_log", "esp_timer"])

    def test_unresolvable_var_passes_through_literally(self):
        cmake = "idf_component_register(REQUIRES ${UNDEFINED_VAR})\n"
        requires, _ = parse_requires(cmake)
        self.assertEqual(requires, ["${UNDEFINED_VAR}"])

    def test_mixed_literal_and_var_tokens(self):
        cmake = (
            "set(BB_SINK_UDP_PRIV_REQUIRES bb_nv bb_log bb_init)\n"
            "idf_component_register(\n"
            "    REQUIRES bb_core bb_pub\n"
            "    PRIV_REQUIRES ${BB_SINK_UDP_PRIV_REQUIRES} bb_udp_frame)\n"
        )
        requires, priv = parse_requires(cmake)
        self.assertEqual(requires, ["bb_core", "bb_pub"])
        self.assertEqual(priv, ["bb_init", "bb_log", "bb_nv", "bb_udp_frame"])


class TestConditionalSetFailsLoud(unittest.TestCase):
    """A `set(VAR ...)` inside an if()/elseif()/else() block can silently
    pick the textually-last branch's value; the parser must refuse to guess
    and raise instead (list(APPEND ...) inside a conditional is still fine —
    only set() is rejected there)."""

    def test_set_inside_if_raises(self):
        cmake = (
            "if(CONFIG_BB_FOO_VARIANT)\n"
            "    set(_foo_reqs bb_a)\n"
            "else()\n"
            "    set(_foo_reqs bb_b)\n"
            "endif()\n"
            "idf_component_register(REQUIRES ${_foo_reqs})\n"
        )
        with self.assertRaises(ConditionalSetError) as ctx:
            parse_requires(cmake, component="bb_foo")
        self.assertIn("bb_foo", str(ctx.exception))
        self.assertIn("_foo_reqs", str(ctx.exception))

    def test_set_inside_if_raises_without_component_name(self):
        cmake = (
            "if(SOME_FLAG)\n"
            "    set(_v bb_a)\n"
            "endif()\n"
            "idf_component_register(REQUIRES ${_v})\n"
        )
        with self.assertRaises(ConditionalSetError):
            parse_requires(cmake)

    def test_list_append_inside_if_still_allowed(self):
        # Unconditional over-approximation for list(APPEND ...) is fine —
        # it only ever adds, never replaces the seeded value.
        cmake = (
            "set(_reqs bb_core)\n"
            "if(CONFIG_BB_FOO_EXTRA)\n"
            "    list(APPEND _reqs bb_extra)\n"
            "endif()\n"
            "idf_component_register(REQUIRES ${_reqs})\n"
        )
        requires, _ = parse_requires(cmake, component="bb_foo")
        self.assertEqual(requires, ["bb_core", "bb_extra"])

    def test_set_outside_any_if_unaffected(self):
        cmake = (
            "set(_reqs bb_core)\n"
            "idf_component_register(REQUIRES ${_reqs})\n"
        )
        requires, _ = parse_requires(cmake, component="bb_foo")
        self.assertEqual(requires, ["bb_core"])

    def test_conditional_set_not_used_in_requires_is_unaffected(self):
        # Real-world shape (bb_sink_ws/bb_i2c/bb_sink_udp): a conditional
        # set() picks a platform-specific SRCS path, never referenced by
        # REQUIRES/PRIV_REQUIRES — must not raise.
        cmake = (
            "if(ESP_PLATFORM)\n"
            "    set(_srcs \"platform/espidf/bb_foo/bb_foo.c\")\n"
            "else()\n"
            "    set(_srcs \"platform/host/bb_foo/bb_foo.c\")\n"
            "endif()\n"
            "idf_component_register(\n"
            "    SRCS ${_srcs}\n"
            "    REQUIRES bb_core\n"
            "    PRIV_REQUIRES bb_log)\n"
        )
        requires, priv = parse_requires(cmake, component="bb_foo")
        self.assertEqual(requires, ["bb_core"])
        self.assertEqual(priv, ["bb_log"])


class TestParseHints(unittest.TestCase):
    def test_single_hint(self):
        cmake = "# bbtool-scaffold-hint: include=platform/host/bb_foo/extra\n"
        hints = parse_hints(cmake)
        self.assertEqual(hints, {"include": ["platform/host/bb_foo/extra"]})

    def test_multiple_hints_same_key_accumulate(self):
        cmake = (
            "# bbtool-scaffold-hint: source=components/bb_foo/legacy/a.c\n"
            "# bbtool-scaffold-hint: source=components/bb_foo/legacy/b.c\n"
        )
        hints = parse_hints(cmake)
        self.assertEqual(
            hints["source"],
            ["components/bb_foo/legacy/a.c", "components/bb_foo/legacy/b.c"],
        )

    def test_no_hints(self):
        self.assertEqual(parse_hints("idf_component_register(REQUIRES bb_core)\n"), {})

    def test_hint_survives_comment_stripping_input(self):
        # parse_hints reads the ORIGINAL text (hints are themselves comments,
        # so they'd vanish under strip_cmake_comments).
        cmake = "# bbtool-scaffold-hint: include=components/bb_foo\n"
        self.assertEqual(strip_cmake_comments(cmake).strip(), "")
        self.assertEqual(parse_hints(cmake), {"include": ["components/bb_foo"]})


if __name__ == "__main__":
    unittest.main()
