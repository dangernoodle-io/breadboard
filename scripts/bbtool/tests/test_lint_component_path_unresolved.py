"""Fixture tests for the component-path-unresolved lint rule (B1-1134):
every filesystem path a component's CMakeLists.txt or an example's
platformio.ini references must actually resolve on disk.

Deliberately fixture-based (a fresh tempdir per test, never the real repo
tree) so these stay stable as the real tree's own layout evolves -- the
acceptance bar is that the rule is OBSERVED FAILING on each defect shape it
exists to catch, plus a clean positive control for each construct."""
import os
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "commands"))

from core import Context
from commands.lint import _check_component_path_unresolved


def make_ctx(root: str) -> Context:
    return Context(root=root, config={})


def _write(path: str, content: str = "") -> None:
    os.makedirs(os.path.dirname(path), exist_ok=True)
    Path(path).write_text(content)


def _make_component(root: str, rel_dir: str, cmake_body: str) -> None:
    """rel_dir is relative to root, e.g. 'components/bb_fake' (flat) or
    'components/display/bb_fake' (grouped) -- discovery's leaf rule treats
    both identically."""
    _write(os.path.join(root, rel_dir, "CMakeLists.txt"), cmake_body)


class TestComponentCMakeListsNegative(unittest.TestCase):
    """Each of these must be OBSERVED FAILING -- a guard never seen to fail
    is not known to work."""

    def test_fires_on_bad_srcs_path(self):
        with tempfile.TemporaryDirectory() as td:
            _make_component(
                td, "components/bb_fake",
                'idf_component_register(\n'
                '    SRCS "${CMAKE_CURRENT_LIST_DIR}/src/does_not_exist.c"\n'
                ')\n',
            )
            violations = _check_component_path_unresolved(make_ctx(td))
            self.assertTrue(violations, "a SRCS path to a missing file must fire")
            self.assertIn("bb_fake", violations[0]["detail"])
            self.assertIn("SRCS", violations[0]["detail"])

    def test_fires_on_bad_include_dirs(self):
        with tempfile.TemporaryDirectory() as td:
            _make_component(
                td, "components/bb_fake",
                'idf_component_register(\n'
                '    INCLUDE_DIRS "include"\n'
                ')\n',
            )
            # note: components/bb_fake/include is never created
            violations = _check_component_path_unresolved(make_ctx(td))
            self.assertTrue(violations, "an INCLUDE_DIRS path to a missing dir must fire")
            self.assertIn("INCLUDE_DIRS", violations[0]["detail"])

    def test_fires_on_bad_src_dirs(self):
        with tempfile.TemporaryDirectory() as td:
            _make_component(
                td, "components/bb_fake",
                'idf_component_register(\n'
                '    SRC_DIRS "src"\n'
                ')\n',
            )
            violations = _check_component_path_unresolved(make_ctx(td))
            self.assertTrue(violations, "a SRC_DIRS path to a missing dir must fire")

    def test_fires_on_bad_priv_include_dirs(self):
        with tempfile.TemporaryDirectory() as td:
            _make_component(
                td, "components/bb_fake",
                'idf_component_register(\n'
                '    PRIV_INCLUDE_DIRS "priv"\n'
                ')\n',
            )
            violations = _check_component_path_unresolved(make_ctx(td))
            self.assertTrue(violations, "a PRIV_INCLUDE_DIRS path to a missing dir must fire")

    def test_fires_on_unresolved_cmake_var(self):
        """A token containing a ${VAR} this rule can't understand must be a
        violation, NEVER a silent skip (the HARD CONSTRAINT)."""
        with tempfile.TemporaryDirectory() as td:
            _make_component(
                td, "components/bb_fake",
                'idf_component_register(\n'
                '    SRCS "${SOME_UNKNOWN_GENERATOR_VAR}/a.c"\n'
                ')\n',
            )
            violations = _check_component_path_unresolved(make_ctx(td))
            self.assertTrue(violations, "an unrecognized ${VAR} in a path token must fire")
            self.assertIn("unresolved CMake variable", violations[0]["detail"])

    def test_moved_grouped_component_relative_srcs_breaks(self):
        """The B1-1134 motivating defect: a component's
        ${CMAKE_CURRENT_LIST_DIR}/../../platform/... SRCS path assumes the
        OLD (flat, one-level-under-components/) nesting depth; grouping it
        under components/<group>/<name>/ makes the relative path resolve
        one level too shallow."""
        with tempfile.TemporaryDirectory() as td:
            # platform/espidf/bb_fake/bb_fake.c exists at the flat-nesting
            # depth (../../platform/... from components/bb_fake/) ...
            _write(os.path.join(td, "platform/espidf/bb_fake/bb_fake.c"), "")
            # ... but the component was grouped one level deeper without
            # updating its relative SRCS path.
            _make_component(
                td, "components/display/bb_fake",
                'idf_component_register(\n'
                '    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_fake/bb_fake.c"\n'
                ')\n',
            )
            violations = _check_component_path_unresolved(make_ctx(td))
            self.assertTrue(violations, "a stale relative SRCS path after grouping must fire")


class TestComponentCMakeListsPositive(unittest.TestCase):
    def test_clean_flat_component_passes(self):
        with tempfile.TemporaryDirectory() as td:
            _write(os.path.join(td, "components/bb_fake/src/bb_fake.c"), "")
            _write(os.path.join(td, "components/bb_fake/include/bb_fake.h"), "")
            _make_component(
                td, "components/bb_fake",
                'idf_component_register(\n'
                '    SRCS "${CMAKE_CURRENT_LIST_DIR}/src/bb_fake.c"\n'
                '    INCLUDE_DIRS "include"\n'
                ')\n',
            )
            violations = _check_component_path_unresolved(make_ctx(td))
            self.assertFalse(violations, "every declared path exists — must not fire")

    def test_clean_grouped_component_with_platform_srcs_passes(self):
        with tempfile.TemporaryDirectory() as td:
            _write(os.path.join(td, "platform/espidf/bb_fake/bb_fake.c"), "")
            _write(os.path.join(td, "components/display/bb_fake/include/bb_fake.h"), "")
            _make_component(
                td, "components/display/bb_fake",
                'idf_component_register(\n'
                '    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../../platform/espidf/bb_fake/bb_fake.c"\n'
                '    INCLUDE_DIRS "include"\n'
                ')\n',
            )
            violations = _check_component_path_unresolved(make_ctx(td))
            self.assertFalse(
                violations,
                "a grouped component whose relative SRCS depth was correctly"
                " updated must not fire",
            )

    def test_clean_src_dirs_component_passes(self):
        with tempfile.TemporaryDirectory() as td:
            _write(os.path.join(td, "components/bb_data/src/bb_data.c"), "")
            _make_component(
                td, "components/bb_data",
                'idf_component_register(\n'
                '    SRC_DIRS "src"\n'
                ')\n',
            )
            violations = _check_component_path_unresolved(make_ctx(td))
            self.assertFalse(violations)

    def test_no_components_dir_no_violations(self):
        with tempfile.TemporaryDirectory() as td:
            violations = _check_component_path_unresolved(make_ctx(td))
            self.assertEqual(violations, [])

    def test_conditional_set_srcs_both_branches_clean_passes(self):
        """A path keyword fed by a genuinely conditional set() does NOT
        skip -- every branch's path is checked. Both branches resolve here,
        so this must be clean."""
        with tempfile.TemporaryDirectory() as td:
            _write(os.path.join(td, "components/bb_fake/espidf.c"), "")
            _write(os.path.join(td, "components/bb_fake/host.c"), "")
            _make_component(
                td, "components/bb_fake",
                'if(IDF_TARGET)\n'
                '    set(_backend "${CMAKE_CURRENT_LIST_DIR}/espidf.c")\n'
                'else()\n'
                '    set(_backend "${CMAKE_CURRENT_LIST_DIR}/host.c")\n'
                'endif()\n'
                'idf_component_register(\n'
                '    SRCS "${_backend}"\n'
                ')\n',
            )
            violations = _check_component_path_unresolved(make_ctx(td))
            self.assertEqual(violations, [])

    def test_conditional_set_bad_path_in_if_branch_fires(self):
        with tempfile.TemporaryDirectory() as td:
            _write(os.path.join(td, "components/bb_fake/host.c"), "")
            # note: espidf.c is never created
            _make_component(
                td, "components/bb_fake",
                'if(IDF_TARGET)\n'
                '    set(_backend "${CMAKE_CURRENT_LIST_DIR}/espidf.c")\n'
                'else()\n'
                '    set(_backend "${CMAKE_CURRENT_LIST_DIR}/host.c")\n'
                'endif()\n'
                'idf_component_register(\n'
                '    SRCS "${_backend}"\n'
                ')\n',
            )
            violations = _check_component_path_unresolved(make_ctx(td))
            self.assertTrue(violations, "a bad path in the if-branch must fire")
            self.assertIn("espidf.c", violations[0]["detail"])

    def test_conditional_set_bad_path_in_else_branch_fires(self):
        """The case that a skip would have missed: the DEFAULT/obvious
        branch (if-branch, IDF_TARGET) is clean, but the non-obvious
        else-branch (host) has a stale path. A skip-on-conditional posture
        would never catch this; branch enumeration does."""
        with tempfile.TemporaryDirectory() as td:
            _write(os.path.join(td, "components/bb_fake/espidf.c"), "")
            # note: host.c is never created -- only reachable via else()
            _make_component(
                td, "components/bb_fake",
                'if(IDF_TARGET)\n'
                '    set(_backend "${CMAKE_CURRENT_LIST_DIR}/espidf.c")\n'
                'else()\n'
                '    set(_backend "${CMAKE_CURRENT_LIST_DIR}/host.c")\n'
                'endif()\n'
                'idf_component_register(\n'
                '    SRCS "${_backend}"\n'
                ')\n',
            )
            violations = _check_component_path_unresolved(make_ctx(td))
            self.assertTrue(
                violations,
                "a bad path in the non-obvious else-branch must fire -- this"
                " is exactly what a ConditionalSetError skip would have"
                " missed",
            )
            self.assertIn("host.c", violations[0]["detail"])


class TestComponentBbEmbedAssets(unittest.TestCase):
    """bb_embed_assets(OUT_SRCS <var> ASSETS <file>:<symbol> ...) --
    real-tree shape bb_prov_default_form. The generated OUT_SRCS .c file is
    MODELED (never checked as a literal on-disk path); its ASSETS input
    file(s) are validated instead."""

    def test_asset_input_exists_passes(self):
        with tempfile.TemporaryDirectory() as td:
            _write(os.path.join(td, "components/bb_fake/form.html"), "")
            _make_component(
                td, "components/bb_fake",
                'bb_embed_assets(\n'
                '    OUT_SRCS _embed_srcs\n'
                '    ASSETS\n'
                '        form.html:bb_fake_form_gz\n'
                ')\n'
                'idf_component_register(\n'
                '    SRCS "src/bb_fake.c" ${_embed_srcs}\n'
                ')\n',
            )
            _write(os.path.join(td, "components/bb_fake/src/bb_fake.c"), "")
            violations = _check_component_path_unresolved(make_ctx(td))
            self.assertEqual(
                violations, [],
                "OUT_SRCS is generated (modeled), not a literal path to"
                " check; the ASSETS input exists, so this must be clean",
            )

    def test_asset_input_missing_fires(self):
        with tempfile.TemporaryDirectory() as td:
            # note: form.html is never created
            _write(os.path.join(td, "components/bb_fake/src/bb_fake.c"), "")
            _make_component(
                td, "components/bb_fake",
                'bb_embed_assets(\n'
                '    OUT_SRCS _embed_srcs\n'
                '    ASSETS\n'
                '        form.html:bb_fake_form_gz\n'
                ')\n'
                'idf_component_register(\n'
                '    SRCS "src/bb_fake.c" ${_embed_srcs}\n'
                ')\n',
            )
            violations = _check_component_path_unresolved(make_ctx(td))
            self.assertTrue(
                violations,
                "a bb_embed_assets ASSETS input that doesn't exist on disk"
                " must fire -- a renamed/deleted asset is a genuine build"
                " break",
            )
            self.assertIn("form.html", violations[0]["detail"])
            self.assertIn("bb_embed_assets", violations[0]["detail"])

    def test_out_srcs_var_never_flagged_as_unresolved_variable(self):
        """Without bb_embed_assets modeling, ${_embed_srcs} would be an
        'unresolved CMake variable' violation (it's never set()/list-
        appended anywhere in this file) -- confirm the modeling actually
        exempts it, distinct from the ASSETS-input check above."""
        with tempfile.TemporaryDirectory() as td:
            _write(os.path.join(td, "components/bb_fake/form.html"), "")
            _make_component(
                td, "components/bb_fake",
                'bb_embed_assets(\n'
                '    OUT_SRCS _embed_srcs\n'
                '    ASSETS\n'
                '        form.html:bb_fake_form_gz\n'
                ')\n'
                'idf_component_register(\n'
                '    SRCS ${_embed_srcs}\n'
                ')\n',
            )
            violations = _check_component_path_unresolved(make_ctx(td))
            self.assertEqual(violations, [])


class TestComponentTargetIncludeDirectories(unittest.TestCase):
    """B1-1134 review HIGH finding: the guard originally only walked
    idf_component_register(...), so target_include_directories(...) (real-
    tree shape: bb_display_ili9341, bb_display_st77xx) was invisible -- a
    group-move that updates SRCS correctly but misses this call would have
    passed clean and failed at CMake configure."""

    def test_fires_on_bad_target_include_directories_path(self):
        with tempfile.TemporaryDirectory() as td:
            _make_component(
                td, "components/bb_fake",
                'idf_component_register(INCLUDE_DIRS "include")\n'
                'target_include_directories(${COMPONENT_LIB} PRIVATE\n'
                '    "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_missing")\n',
            )
            _write(os.path.join(td, "components/bb_fake/include/bb_fake.h"), "")
            violations = _check_component_path_unresolved(make_ctx(td))
            self.assertTrue(
                violations,
                "a target_include_directories(...) path to a missing dir must fire",
            )
            self.assertIn("target_include_directories", violations[0]["detail"])

    def test_clean_target_include_directories_passes(self):
        with tempfile.TemporaryDirectory() as td:
            _write(os.path.join(td, "platform/espidf/bb_spi_common/bb_spi_common.h"), "")
            _write(os.path.join(td, "components/bb_fake/include/bb_fake.h"), "")
            _make_component(
                td, "components/bb_fake",
                'idf_component_register(INCLUDE_DIRS "include")\n'
                'target_include_directories(${COMPONENT_LIB} PRIVATE\n'
                '    "${CMAKE_CURRENT_LIST_DIR}/../../platform/espidf/bb_spi_common")\n',
            )
            violations = _check_component_path_unresolved(make_ctx(td))
            self.assertEqual(violations, [])

    def test_opaque_property_var_exempted_not_flagged(self):
        """Real-tree shape (bb_diag): idf_component_get_property(...)
        assigns a var from ESP-IDF's own build-graph state -- a path token
        referencing it must be exempted (modeled), not flagged as an
        unresolved variable and not silently ignored either (it's a
        DOCUMENTED exclusion, verified here)."""
        with tempfile.TemporaryDirectory() as td:
            _make_component(
                td, "components/bb_fake",
                'idf_component_register()\n'
                'idf_component_get_property(espcoredump_dir espcoredump COMPONENT_DIR)\n'
                'target_include_directories(${COMPONENT_LIB} PRIVATE '
                '${espcoredump_dir}/include)\n',
            )
            violations = _check_component_path_unresolved(make_ctx(td))
            self.assertEqual(
                violations, [],
                "a path token referencing an idf_component_get_property"
                " var must be exempted, not flagged",
            )

    def test_reference_before_property_call_not_exempted(self):
        """B1-1134 review HIGH, reproduced case: a fabricated path
        referencing a var name that IS assigned by a property-get call
        SOMEWHERE in the file, but the reference textually PRECEDES that
        call, must NOT be exempted -- a flat, position-blind name match
        would have silently passed this fabricated path clean."""
        with tempfile.TemporaryDirectory() as td:
            _make_component(
                td, "components/bb_fake",
                'idf_component_register(SRCS "${totally_bogus_dir}/nope.c")\n'
                'idf_component_get_property(totally_bogus_dir some_other_component '
                'COMPONENT_DIR)\n',
            )
            violations = _check_component_path_unresolved(make_ctx(td))
            self.assertTrue(
                violations,
                "a reference preceding its var's property-get call must"
                " fire, not be silently exempted",
            )
            self.assertIn("totally_bogus_dir", violations[0]["detail"])

    def test_var_reassigned_by_set_after_property_call_not_exempted(self):
        """A var whose property-get is later shadowed by a set() for the
        SAME name (e.g. accidental reuse of a short name like `dir`) must
        not be exempted for a reference that follows the property-get but
        precedes (or follows) the set() -- the var's assignment history is
        no longer "exactly one property call", so it never qualifies at
        all."""
        with tempfile.TemporaryDirectory() as td:
            _make_component(
                td, "components/bb_fake",
                'idf_component_get_property(shared_var some_component SOME_PROP)\n'
                'set(shared_var "totally/unrelated/nope.c")\n'
                'idf_component_register(SRCS "${shared_var}")\n',
            )
            violations = _check_component_path_unresolved(make_ctx(td))
            self.assertTrue(
                violations,
                "a var reassigned by set() after its property-get must"
                " never be exempted",
            )

    def test_var_assigned_by_both_property_call_and_set_not_exempted(self):
        with tempfile.TemporaryDirectory() as td:
            _make_component(
                td, "components/bb_fake",
                'set(shared_var "totally/unrelated/nope.c")\n'
                'idf_component_get_property(shared_var some_component SOME_PROP)\n'
                'idf_component_register(SRCS "${shared_var}")\n',
            )
            violations = _check_component_path_unresolved(make_ctx(td))
            self.assertTrue(
                violations,
                "a var assigned by both a property call and a set() must"
                " never be exempted, regardless of order",
            )

    def test_legitimate_property_call_then_reference_still_exempted(self):
        """The one case that SHOULD be exempted: a property-get call, then
        a reference, and no other assignment anywhere in the file."""
        with tempfile.TemporaryDirectory() as td:
            _make_component(
                td, "components/bb_fake",
                'idf_component_get_property(espcoredump_dir espcoredump '
                'COMPONENT_DIR)\n'
                'idf_component_register(SRCS "${espcoredump_dir}/panic.c")\n',
            )
            violations = _check_component_path_unresolved(make_ctx(td))
            self.assertEqual(
                violations, [],
                "property-get, then reference, no other assignment --"
                " must still be exempted",
            )


class TestComponentIncludeCalls(unittest.TestCase):
    """B1-1134 review HIGH finding: components/bb_prov_default_form/
    CMakeLists.txt:1's include(...) with a depth-dependent relative path
    breaks the same way a stale SRCS path does on a group-move."""

    def test_fires_on_bad_include_path(self):
        with tempfile.TemporaryDirectory() as td:
            _make_component(
                td, "components/bb_fake",
                'include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/missing.cmake")\n'
                'idf_component_register()\n',
            )
            violations = _check_component_path_unresolved(make_ctx(td))
            self.assertTrue(violations, "an include(...) path to a missing file must fire")
            self.assertIn("include", violations[0]["detail"])

    def test_clean_include_passes(self):
        with tempfile.TemporaryDirectory() as td:
            _write(os.path.join(td, "cmake/bbtool.cmake"), "")
            _make_component(
                td, "components/bb_fake",
                'include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/bbtool.cmake")\n'
                'idf_component_register()\n',
            )
            violations = _check_component_path_unresolved(make_ctx(td))
            self.assertEqual(violations, [])

    def test_bare_module_name_not_flagged(self):
        """A CMake built-in module name (CMAKE_MODULE_PATH resolution) is
        not a same-tree relative path -- must not be misinterpreted as a
        broken one."""
        with tempfile.TemporaryDirectory() as td:
            _make_component(
                td, "components/bb_fake",
                'include(GNUInstallDirs)\n'
                'idf_component_register()\n',
            )
            violations = _check_component_path_unresolved(make_ctx(td))
            self.assertEqual(violations, [])


class TestPlatformioIniNegative(unittest.TestCase):
    def _make_smoke_ini(self, td: str, build_flags: str, build_src_filter: str) -> None:
        _write(os.path.join(td, "examples/smoke/main/entry.c"), "")
        _write(
            os.path.join(td, "examples/smoke/platformio.ini"),
            "[platformio]\n"
            "default_envs = env1\n"
            "src_dir = main\n\n"
            "[env:env1]\n"
            f"build_flags =\n{build_flags}\n"
            f"build_src_filter =\n{build_src_filter}\n",
        )

    def test_fires_on_bad_include_dir(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_smoke_ini(
                td,
                build_flags="    -I../../components/does_not_exist/include\n",
                build_src_filter="    +<*>\n",
            )
            violations = _check_component_path_unresolved(make_ctx(td))
            self.assertTrue(violations, "a -I flag to a missing dir must fire")
            self.assertIn("build_flags", violations[0]["detail"])

    def test_fires_on_build_src_filter_zero_match(self):
        with tempfile.TemporaryDirectory() as td:
            _write(os.path.join(td, "examples/smoke/include/dummy.h"), "")
            self._make_smoke_ini(
                td,
                build_flags="    -Iinclude\n",
                build_src_filter="    +<../../../platform/host/bb_missing/*.c>\n",
            )
            violations = _check_component_path_unresolved(make_ctx(td))
            self.assertTrue(
                violations,
                "a build_src_filter +<...> pattern matching zero files must fire"
                " -- PlatformIO itself does NOT error on this, which is exactly"
                " why the rule exists",
            )
            self.assertIn("build_src_filter", violations[0]["detail"])
            self.assertIn("matches zero files", violations[0]["detail"])

    def test_fires_on_malformed_build_src_filter_entry(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_smoke_ini(
                td,
                build_flags="    -Iinclude\n",
                build_src_filter="    +<unterminated\n",
            )
            _write(os.path.join(td, "examples/smoke/include/dummy.h"), "")
            violations = _check_component_path_unresolved(make_ctx(td))
            self.assertTrue(violations, "a malformed +<...> entry (no closing '>') must fire")
            self.assertIn("malformed", violations[0]["detail"])


class TestPlatformioIniPositive(unittest.TestCase):
    def test_clean_ini_passes(self):
        with tempfile.TemporaryDirectory() as td:
            _write(os.path.join(td, "components/bb_fake/include/bb_fake.h"), "")
            _write(os.path.join(td, "platform/host/bb_fake/bb_fake.c"), "")
            _write(os.path.join(td, "examples/smoke/main/entry.c"), "")
            _write(
                os.path.join(td, "examples/smoke/platformio.ini"),
                "[platformio]\n"
                "default_envs = env1\n"
                "src_dir = main\n\n"
                "[env:env1]\n"
                "build_flags =\n"
                "    -I../../components/bb_fake/include\n\n"
                "build_src_filter =\n"
                "    +<*>\n"
                "    +<../../../platform/host/bb_fake/*.c>\n",
            )
            violations = _check_component_path_unresolved(make_ctx(td))
            self.assertFalse(violations, "every -I dir and +<> pattern resolves — must not fire")

    def test_no_examples_dir_no_violations(self):
        with tempfile.TemporaryDirectory() as td:
            violations = _check_component_path_unresolved(make_ctx(td))
            self.assertEqual(violations, [])

    def test_exclude_pattern_never_required_to_resolve(self):
        """A -<...> exclusion entry naming an already-absent file is
        harmless (there's nothing to exclude) — only +<...> entries are
        required to match >=1 file."""
        with tempfile.TemporaryDirectory() as td:
            _write(os.path.join(td, "examples/smoke/main/entry.c"), "")
            _write(os.path.join(td, "examples/smoke/include/dummy.h"), "")
            _write(
                os.path.join(td, "examples/smoke/platformio.ini"),
                "[platformio]\n"
                "default_envs = env1\n"
                "src_dir = main\n\n"
                "[env:env1]\n"
                "build_flags =\n"
                "    -Iinclude\n\n"
                "build_src_filter =\n"
                "    +<*>\n"
                "    -<never_existed.c>\n",
            )
            violations = _check_component_path_unresolved(make_ctx(td))
            self.assertFalse(violations)


class TestCIncludeIntoComponents(unittest.TestCase):
    """B1-1140: a `#include "..."` relative path that resolves into
    components/ must be checked the same way a CMakeLists.txt path is --
    resolved relative to the INCLUDING FILE's own directory, not the
    component's directory."""

    def test_fires_on_stale_test_fixture_include(self):
        """Real-tree shape (B1-980): test/test_host/test_main.c reaching
        into a component's internal test header via a relative path that
        breaks after the component moves."""
        with tempfile.TemporaryDirectory() as td:
            _write(
                os.path.join(td, "test/test_host/test_main.c"),
                '#include "../../components/bb_display/bb_display_test.h"\n',
            )
            # note: components/bb_display/bb_display_test.h is never created
            violations = _check_component_path_unresolved(make_ctx(td))
            self.assertTrue(
                violations,
                "a stale #include reaching into components/ must fire",
            )
            self.assertIn("bb_display_test.h", violations[0]["detail"])
            self.assertIn("does not exist", violations[0]["detail"])

    def test_fires_on_stale_platform_backend_include(self):
        """Real-tree shape (B1-980): platform/espidf/bb_led_gpio/
        bb_led_gpio.c reaching into its own component's internal header via
        a depth-dependent relative path that breaks after a group-move."""
        with tempfile.TemporaryDirectory() as td:
            _write(
                os.path.join(td, "platform/espidf/bb_led_gpio/bb_led_gpio.c"),
                '#include "../../../components/bb_led_gpio/'
                'bb_led_gpio_internal.h"\n',
            )
            # note: the grouped component now lives one level deeper
            _write(
                os.path.join(
                    td,
                    "components/display/bb_led_gpio/bb_led_gpio_internal.h",
                ),
                "",
            )
            violations = _check_component_path_unresolved(make_ctx(td))
            self.assertTrue(
                violations,
                "a #include whose relative depth is stale after a"
                " group-move must fire",
            )
            self.assertIn("bb_led_gpio_internal.h", violations[0]["detail"])

    def test_clean_include_into_components_passes(self):
        with tempfile.TemporaryDirectory() as td:
            _write(
                os.path.join(
                    td, "components/bb_led_gpio/bb_led_gpio_internal.h",
                ),
                "",
            )
            _write(
                os.path.join(td, "platform/espidf/bb_led_gpio/bb_led_gpio.c"),
                '#include "../../../components/bb_led_gpio/'
                'bb_led_gpio_internal.h"\n',
            )
            violations = _check_component_path_unresolved(make_ctx(td))
            self.assertEqual(
                violations, [],
                "a #include into components/ whose target exists must not"
                " fire",
            )

    def test_include_not_reaching_into_components_out_of_scope(self):
        """A same-directory or platform-local #include that never resolves
        into components/ is out of this check's scope, even though the
        target file doesn't exist -- that's a compiler-time concern, not
        this rule's."""
        with tempfile.TemporaryDirectory() as td:
            _write(
                os.path.join(td, "platform/espidf/bb_led_gpio/bb_led_gpio.c"),
                '#include "bb_led_gpio_local.h"\n',
            )
            # note: bb_led_gpio_local.h is never created, and never resolves
            # into components/ -- must not fire under this rule
            violations = _check_component_path_unresolved(make_ctx(td))
            self.assertEqual(violations, [])

    def test_bare_filename_include_search_path_not_flagged(self):
        """Real-tree shape: a public header includes a dependency's header
        by bare filename (e.g. #include "bb_core.h"), resolved via the
        component's INCLUDE_DIRS/REQUIRES search path -- NOT a literal
        relative-path navigation. This rule must not guess at that
        resolution (it would need the full dependency graph) and must not
        flag it, even though the naive same-dir join doesn't exist."""
        with tempfile.TemporaryDirectory() as td:
            _write(
                os.path.join(td, "components/bb_task/include/bb_task.h"),
                '#include "bb_core.h"\n',
            )
            # note: components/bb_task/include/bb_core.h never exists --
            # bb_core.h actually lives in a different component's include/
            violations = _check_component_path_unresolved(make_ctx(td))
            self.assertEqual(
                violations, [],
                "a bare-filename #include (no '/') must never be flagged"
                " -- it's resolved via the compiler's include search path,"
                " not a relative-path join",
            )

    def test_quoted_esp_idf_sdk_header_not_flagged(self):
        """Real-tree shape (bb_lifecycle, bb_log, bb_core): ESP-IDF's own
        convention is to quote its SDK headers too (#include
        "freertos/FreeRTOS.h", #include "driver/i2c_master.h"). These are
        search-path-resolved (the ESP-IDF build injects freertos/ and
        driver/ into the include search path), NOT relative to the
        including file's own directory -- a naive same-directory join
        would land under components/ purely because the including file
        already lives there, which must NOT be flagged."""
        with tempfile.TemporaryDirectory() as td:
            _write(
                os.path.join(td, "components/bb_core/include/bb_once.h"),
                '#include "freertos/FreeRTOS.h"\n'
                '#include "driver/i2c_master.h"\n',
            )
            violations = _check_component_path_unresolved(make_ctx(td))
            self.assertEqual(
                violations, [],
                "a quoted ESP-IDF SDK header (no literal 'components/' in"
                " the token) must never be flagged",
            )

    def test_angle_bracket_include_never_checked(self):
        with tempfile.TemporaryDirectory() as td:
            _write(
                os.path.join(td, "components/bb_fake/src/bb_fake.c"),
                '#include <stdio.h>\n',
            )
            violations = _check_component_path_unresolved(make_ctx(td))
            self.assertEqual(violations, [])

    def test_include_of_own_component_header_exists_passes(self):
        """The common, everyday case: a component's own .c including its
        own public header by a short relative path -- must never fire when
        the header exists."""
        with tempfile.TemporaryDirectory() as td:
            _write(os.path.join(td, "components/bb_fake/include/bb_fake.h"), "")
            _write(
                os.path.join(td, "components/bb_fake/src/bb_fake.c"),
                '#include "../include/bb_fake.h"\n',
            )
            violations = _check_component_path_unresolved(make_ctx(td))
            self.assertEqual(violations, [])


if __name__ == "__main__":
    unittest.main()
