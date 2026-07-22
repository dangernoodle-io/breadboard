"""cmake_parse tests: REQUIRES/PRIV_REQUIRES parsing (incl. set()/list(APPEND)
variable indirection) + bbtool-scaffold-hint comment parsing."""
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from cmake_parse import (
    ConditionalSetError,
    parse_embed_assets,
    parse_hints,
    parse_include_calls,
    parse_paths,
    parse_requires,
    parse_target_include_directories,
    parse_var_assignment_events,
    single_opaque_property_vars,
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


class TestParsePathsLiteral(unittest.TestCase):
    def test_all_four_keywords(self):
        cmake = (
            "idf_component_register(\n"
            '    SRCS "${CMAKE_CURRENT_LIST_DIR}/src/a.c"\n'
            '    SRC_DIRS "src"\n'
            '    INCLUDE_DIRS "include"\n'
            '    PRIV_INCLUDE_DIRS "priv_include"\n'
            "    REQUIRES bb_core\n"
            ")\n"
        )
        paths = parse_paths(cmake, component="bb_fake")
        self.assertEqual(paths["SRCS"], ["${CMAKE_CURRENT_LIST_DIR}/src/a.c"])
        self.assertEqual(paths["SRC_DIRS"], ["src"])
        self.assertEqual(paths["INCLUDE_DIRS"], ["include"])
        self.assertEqual(paths["PRIV_INCLUDE_DIRS"], ["priv_include"])

    def test_no_register_call_returns_all_empty(self):
        paths = parse_paths("# nothing here\n")
        self.assertEqual(
            paths,
            {"SRCS": [], "SRC_DIRS": [], "INCLUDE_DIRS": [], "PRIV_INCLUDE_DIRS": []},
        )

    def test_no_path_args_returns_empty_lists(self):
        cmake = "idf_component_register(REQUIRES bb_core)\n"
        paths = parse_paths(cmake)
        self.assertEqual(paths["SRCS"], [])

    def test_multiple_srcs_preserve_order_and_duplicates(self):
        # Unlike parse_requires, path order/duplication is a caller concern
        # (existence-checking), not something this parser collapses.
        cmake = (
            "idf_component_register(\n"
            '    SRCS "a.c" "b.c" "a.c"\n'
            ")\n"
        )
        paths = parse_paths(cmake)
        self.assertEqual(paths["SRCS"], ["a.c", "b.c", "a.c"])

    def test_quoted_cmake_current_list_dir_token_not_mangled(self):
        # A real-tree shape (bb_board, bb_cache_routes, ...): each SRCS entry
        # is individually double-quoted -- the quotes must be stripped before
        # the token is returned, not fused into the path itself.
        cmake = (
            "idf_component_register(\n"
            '    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_x/bb_x.c"\n'
            ")\n"
        )
        paths = parse_paths(cmake)
        self.assertEqual(
            paths["SRCS"],
            ["${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_x/bb_x.c"],
        )


class TestParsePathsVarIndirection(unittest.TestCase):
    def test_set_then_srcs_var(self):
        cmake = (
            "set(_srcs \"${CMAKE_CURRENT_LIST_DIR}/a.c\")\n"
            "idf_component_register(SRCS ${_srcs})\n"
        )
        paths = parse_paths(cmake)
        self.assertEqual(paths["SRCS"], ["${CMAKE_CURRENT_LIST_DIR}/a.c"])

    def test_list_append_extends_srcs_var(self):
        cmake = (
            "set(_srcs \"a.c\")\n"
            "if(CONFIG_BB_FOO_EXTRA)\n"
            "    list(APPEND _srcs \"b.c\")\n"
            "endif()\n"
            "idf_component_register(SRCS ${_srcs})\n"
        )
        paths = parse_paths(cmake)
        self.assertEqual(paths["SRCS"], ["a.c", "b.c"])

    def test_quoted_var_token_still_expands(self):
        # Real-tree shape (bb_bqueue): the ${VAR} SRCS entry is itself
        # double-quoted ("${BACKEND}") -- must still expand via resolved_vars,
        # not fall through to the literal quoted text.
        cmake = (
            "set(_backend \"${CMAKE_CURRENT_LIST_DIR}/platform/espidf/bb_x.c\")\n"
            "idf_component_register(\n"
            '    SRCS "${_backend}"\n'
            ")\n"
        )
        paths = parse_paths(cmake)
        self.assertEqual(
            paths["SRCS"], ["${CMAKE_CURRENT_LIST_DIR}/platform/espidf/bb_x.c"]
        )

    def test_unresolvable_var_passes_through_literally(self):
        cmake = "idf_component_register(SRCS ${UNDEFINED_VAR})\n"
        paths = parse_paths(cmake)
        self.assertEqual(paths["SRCS"], ["${UNDEFINED_VAR}"])


class TestParsePathsConditionalBranchEnumeration(unittest.TestCase):
    """A path keyword fed by a genuinely conditional set() (two DIFFERENT
    single-value backends, e.g. bb_bqueue's real BB_BQUEUE_BACKEND) does NOT
    raise -- unlike parse_requires, there is nothing to guess: every
    branch's path must resolve on disk regardless of which is build-time-
    live, so parse_paths enumerates every branch instead of picking one."""

    def test_set_inside_if_enumerates_both_branches_for_srcs(self):
        cmake = (
            "if(IDF_TARGET)\n"
            "    set(_backend \"platform/espidf/bb_x.c\")\n"
            "else()\n"
            "    set(_backend \"platform/host/bb_x.c\")\n"
            "endif()\n"
            "idf_component_register(SRCS \"${_backend}\")\n"
        )
        paths = parse_paths(cmake, component="bb_x")
        self.assertEqual(
            sorted(paths["SRCS"]),
            ["platform/espidf/bb_x.c", "platform/host/bb_x.c"],
        )

    def test_three_way_if_elseif_else_enumerates_all_three_branches(self):
        cmake = (
            "if(A)\n"
            "    set(_backend \"a.c\")\n"
            "elseif(B)\n"
            "    set(_backend \"b.c\")\n"
            "else()\n"
            "    set(_backend \"c.c\")\n"
            "endif()\n"
            "idf_component_register(SRCS ${_backend})\n"
        )
        paths = parse_paths(cmake, component="bb_x")
        self.assertEqual(sorted(paths["SRCS"]), ["a.c", "b.c", "c.c"])

    def test_unconditional_set_then_conditional_override_enumerates_both(self):
        # Real-tree shape (bb_i2c): an unconditional default set() followed
        # by a conditional override inside if() -- both are real branches.
        cmake = (
            "set(_backend \"platform/host/bb_x.c\")\n"
            "if(IDF_TARGET)\n"
            "    set(_backend \"platform/espidf/bb_x.c\")\n"
            "endif()\n"
            "idf_component_register(SRCS ${_backend})\n"
        )
        paths = parse_paths(cmake, component="bb_x")
        self.assertEqual(
            sorted(paths["SRCS"]),
            ["platform/espidf/bb_x.c", "platform/host/bb_x.c"],
        )

    def test_list_append_inside_if_still_allowed_for_srcs(self):
        cmake = (
            "set(_srcs \"a.c\")\n"
            "if(CONFIG_BB_X_EXTRA)\n"
            "    list(APPEND _srcs \"b.c\")\n"
            "endif()\n"
            "idf_component_register(SRCS ${_srcs})\n"
        )
        paths = parse_paths(cmake, component="bb_x")
        self.assertEqual(paths["SRCS"], ["a.c", "b.c"])

    def test_list_append_inside_if_applies_to_every_conditional_branch(self):
        # An unconditional list(APPEND) on a var that's ALSO conditionally
        # set() must extend every branch, not just one -- the same
        # unconditional over-approximation _resolve_vars already documents.
        cmake = (
            "if(IDF_TARGET)\n"
            "    set(_backend \"espidf.c\")\n"
            "else()\n"
            "    set(_backend \"host.c\")\n"
            "endif()\n"
            "list(APPEND _backend \"extra.c\")\n"
            "idf_component_register(SRCS ${_backend})\n"
        )
        paths = parse_paths(cmake, component="bb_x")
        self.assertEqual(
            sorted(paths["SRCS"]),
            ["espidf.c", "extra.c", "extra.c", "host.c"],
        )

    def test_list_append_before_conditional_set_still_extends_every_branch(self):
        """The untested mirror of test_list_append_inside_if_applies_to_...:
        an append that textually PRECEDES the var's conditional set()
        branches must still extend those branches (not seed an orphan
        phantom branch of its own and leave the real branches
        unextended) -- this was a real ordering bug in an earlier
        single-pass _resolve_vars_branches."""
        cmake = (
            "list(APPEND _backend \"extra.c\")\n"
            "if(IDF_TARGET)\n"
            "    set(_backend \"espidf.c\")\n"
            "else()\n"
            "    set(_backend \"host.c\")\n"
            "endif()\n"
            "idf_component_register(SRCS ${_backend})\n"
        )
        paths = parse_paths(cmake, component="bb_x")
        self.assertEqual(
            sorted(paths["SRCS"]),
            ["espidf.c", "extra.c", "extra.c", "host.c"],
        )

    def test_parse_requires_unaffected_still_raises(self):
        """parse_requires's ConditionalSetError posture is untouched by the
        parse_paths branch-enumeration change."""
        cmake = (
            "if(A)\n"
            "    set(_reqs bb_a)\n"
            "else()\n"
            "    set(_reqs bb_b)\n"
            "endif()\n"
            "idf_component_register(REQUIRES ${_reqs})\n"
        )
        with self.assertRaises(ConditionalSetError):
            parse_requires(cmake, component="bb_x")


class TestParseTargetIncludeDirectories(unittest.TestCase):
    def test_single_call_public(self):
        cmake = (
            'target_include_directories(${COMPONENT_LIB} PRIVATE\n'
            '    "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_x")\n'
        )
        dirs = parse_target_include_directories(cmake)
        self.assertEqual(dirs, ["${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_x"])

    def test_no_call_returns_empty_list(self):
        self.assertEqual(parse_target_include_directories("idf_component_register()\n"), [])

    def test_multiple_dirs_one_call(self):
        cmake = (
            'target_include_directories(${COMPONENT_LIB} PRIVATE "a" "b")\n'
        )
        self.assertEqual(parse_target_include_directories(cmake), ["a", "b"])

    def test_multiple_calls_accumulate_in_source_order(self):
        cmake = (
            'target_include_directories(${COMPONENT_LIB} PRIVATE "a")\n'
            'target_include_directories(${COMPONENT_LIB} PRIVATE "b")\n'
        )
        self.assertEqual(parse_target_include_directories(cmake), ["a", "b"])

    def test_var_token_expands(self):
        cmake = (
            'set(_dir "${CMAKE_CURRENT_LIST_DIR}/foo")\n'
            'target_include_directories(${COMPONENT_LIB} PRIVATE ${_dir})\n'
        )
        self.assertEqual(
            parse_target_include_directories(cmake), ["${CMAKE_CURRENT_LIST_DIR}/foo"]
        )

    def test_opaque_property_var_token_passes_through_unexpanded(self):
        # Real-tree shape (bb_diag): ${espcoredump_dir}/include -- the var
        # is never set()/list-appended anywhere, so it passes through
        # unexpanded (single_opaque_property_vars is what models it as
        # excluded downstream, not this parser).
        cmake = (
            "idf_component_get_property(espcoredump_dir espcoredump COMPONENT_DIR)\n"
            "target_include_directories(${COMPONENT_LIB} PRIVATE ${espcoredump_dir}/include)\n"
        )
        self.assertEqual(
            parse_target_include_directories(cmake), ["${espcoredump_dir}/include"]
        )


class TestParseIncludeCalls(unittest.TestCase):
    def test_path_shaped_include_returned(self):
        cmake = 'include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bbtool.cmake")\n'
        self.assertEqual(
            parse_include_calls(cmake),
            ["${CMAKE_CURRENT_LIST_DIR}/../../cmake/bbtool.cmake"],
        )

    def test_bare_module_name_excluded(self):
        # No '/' anywhere -- a CMake built-in module name (CMAKE_MODULE_PATH
        # resolution), not a same-tree relative path; must not be
        # misinterpreted as a broken relative path.
        cmake = "include(GNUInstallDirs)\n"
        self.assertEqual(parse_include_calls(cmake), [])

    def test_no_call_returns_empty_list(self):
        self.assertEqual(parse_include_calls("idf_component_register()\n"), [])

    def test_multiple_calls_accumulate(self):
        cmake = (
            'include("${CMAKE_CURRENT_LIST_DIR}/a.cmake")\n'
            'include("${CMAKE_CURRENT_LIST_DIR}/b.cmake")\n'
        )
        self.assertEqual(
            parse_include_calls(cmake),
            [
                "${CMAKE_CURRENT_LIST_DIR}/a.cmake",
                "${CMAKE_CURRENT_LIST_DIR}/b.cmake",
            ],
        )


class TestParseVarAssignmentEvents(unittest.TestCase):
    def test_single_property_get_event(self):
        cmake = "idf_component_get_property(espcoredump_dir espcoredump COMPONENT_DIR)\n"
        events = parse_var_assignment_events(cmake)
        self.assertEqual(len(events["espcoredump_dir"]), 1)
        self.assertEqual(events["espcoredump_dir"][0][1], "property")

    def test_set_and_append_and_property_all_distinguished(self):
        cmake = (
            "set(_v \"a\")\n"
            "list(APPEND _v \"b\")\n"
            "idf_component_get_property(_v some_component SOME_PROP)\n"
        )
        events = parse_var_assignment_events(cmake)
        kinds_in_order = [kind for _pos, kind in events["_v"]]
        self.assertEqual(kinds_in_order, ["set", "append", "property"])

    def test_positions_are_source_ordered(self):
        cmake = (
            "idf_component_get_property(_v some_component SOME_PROP)\n"
            "set(_v \"a\")\n"
        )
        events = parse_var_assignment_events(cmake)
        positions = [pos for pos, _kind in events["_v"]]
        self.assertEqual(positions, sorted(positions))

    def test_no_events_for_unassigned_var(self):
        self.assertEqual(parse_var_assignment_events("idf_component_register()\n"), {})


class TestSingleOpaquePropertyVars(unittest.TestCase):
    """B1-1134 review HIGH: the exemption must be provenance- AND
    order-aware, never a flat "was this NAME ever assigned by a property
    call anywhere" check -- these are exactly the cases that check got
    wrong."""

    def test_idf_component_get_property_sole_assignment_qualifies(self):
        cmake = "idf_component_get_property(espcoredump_dir espcoredump COMPONENT_DIR)\n"
        result = single_opaque_property_vars(cmake)
        self.assertIn("espcoredump_dir", result)

    def test_idf_build_get_property_sole_assignment_qualifies(self):
        cmake = "idf_build_get_property(some_dir SOME_PROP)\n"
        result = single_opaque_property_vars(cmake)
        self.assertIn("some_dir", result)

    def test_no_call_returns_empty_dict(self):
        self.assertEqual(single_opaque_property_vars("idf_component_register()\n"), {})

    def test_idf_build_set_property_not_matched(self):
        # A SETTER, not a GETTER -- must not be mistaken for one.
        cmake = 'idf_build_set_property(COMPILE_OPTIONS "-DFOO=1" APPEND)\n'
        self.assertEqual(single_opaque_property_vars(cmake), {})

    def test_var_reassigned_by_set_after_property_call_disqualified(self):
        """The reproduced defect's shape, mirrored: even though the
        property call comes first, a LATER set() for the same name means
        the var's assignment history is no longer "exactly one property
        call" -- must NOT qualify."""
        cmake = (
            "idf_component_get_property(shared_var some_component SOME_PROP)\n"
            "set(shared_var \"totally/unrelated/path\")\n"
        )
        self.assertEqual(single_opaque_property_vars(cmake), {})

    def test_var_assigned_by_both_property_and_set_in_either_order_disqualified(self):
        cmake = (
            "set(shared_var \"a\")\n"
            "idf_component_get_property(shared_var some_component SOME_PROP)\n"
        )
        self.assertEqual(single_opaque_property_vars(cmake), {})

    def test_var_with_two_property_calls_disqualified(self):
        cmake = (
            "idf_component_get_property(shared_var comp_a SOME_PROP)\n"
            "idf_component_get_property(shared_var comp_b OTHER_PROP)\n"
        )
        self.assertEqual(single_opaque_property_vars(cmake), {})

    def test_qualifying_var_position_precedes_a_later_text_offset(self):
        """The legitimate case's position bookkeeping: the recorded
        position for a qualifying var is the property-get call's own
        offset, usable by a caller to confirm a later reference follows
        it."""
        cmake = (
            "# leading comment\n"
            "idf_component_get_property(espcoredump_dir espcoredump COMPONENT_DIR)\n"
            "target_include_directories(${COMPONENT_LIB} PRIVATE ${espcoredump_dir}/include)\n"
        )
        result = single_opaque_property_vars(cmake)
        prop_pos = result["espcoredump_dir"]
        stripped = strip_cmake_comments(cmake)
        ref_pos = stripped.find("${espcoredump_dir}/include")
        self.assertGreater(ref_pos, prop_pos)


class TestParseEmbedAssets(unittest.TestCase):
    """bb_embed_assets(OUT_SRCS <var> ASSETS <file>:<symbol> ...) -- the
    configure-time asset-embed macro (cmake/bbtool.cmake). Real-tree shape:
    bb_prov_default_form."""

    def test_single_asset(self):
        cmake = (
            "bb_embed_assets(\n"
            "    OUT_SRCS _embed_srcs\n"
            "    ASSETS\n"
            "        prov_default_form.html:bb_prov_default_form_gz\n"
            ")\n"
        )
        result = parse_embed_assets(cmake)
        self.assertEqual(result, {"_embed_srcs": ["prov_default_form.html"]})

    def test_multiple_assets_one_call(self):
        cmake = (
            "bb_embed_assets(\n"
            "    OUT_SRCS _embed_srcs\n"
            "    ASSETS\n"
            "        a.html:sym_a\n"
            "        b.html:sym_b\n"
            ")\n"
        )
        result = parse_embed_assets(cmake)
        self.assertEqual(result["_embed_srcs"], ["a.html", "b.html"])

    def test_no_call_returns_empty_dict(self):
        self.assertEqual(parse_embed_assets("idf_component_register()\n"), {})

    def test_two_calls_different_out_vars(self):
        cmake = (
            "bb_embed_assets(OUT_SRCS _a ASSETS a.html:sym_a)\n"
            "bb_embed_assets(OUT_SRCS _b ASSETS b.html:sym_b)\n"
        )
        result = parse_embed_assets(cmake)
        self.assertEqual(result, {"_a": ["a.html"], "_b": ["b.html"]})

    def test_two_calls_same_out_var_accumulate(self):
        cmake = (
            "bb_embed_assets(OUT_SRCS _embed_srcs ASSETS a.html:sym_a)\n"
            "bb_embed_assets(OUT_SRCS _embed_srcs ASSETS b.html:sym_b)\n"
        )
        result = parse_embed_assets(cmake)
        self.assertEqual(result["_embed_srcs"], ["a.html", "b.html"])


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
