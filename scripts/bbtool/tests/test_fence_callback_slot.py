"""fence.callback_slot family tests: hand-rolled single-slot-injected-
callback idiom scanning (fires + does not fire), the two shape/arity
exclusions specific to this family, family auto-discovery, and the
shrink-only baseline semantics (via the generic `fence` CLI) applied to
this concrete family."""
import argparse
import contextlib
import io
import os
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "commands"))

import fence as fence_pkg  # noqa: E402
from fence import Marker  # noqa: E402
from fence.callback_slot import scan_all, counts_by_bucket, _component_of  # noqa: E402
from commands import fence_cmd  # noqa: E402
from discovery import build_index  # noqa: E402


def _write(root: Path, rel: str, content: str) -> Path:
    path = root / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")
    _ensure_component_cmakelists(root, rel)
    return path


def _ensure_component_cmakelists(root: Path, rel: str) -> None:
    """Every real `components/<name>/` in this repo has a direct
    `CMakeLists.txt` — discovery.py's leaf rule (depth-agnostic-layout
    lane, PR1) requires one to discover a component at all. These
    fixtures model a synthetic `components/<name>/...` tree with only
    source/header files, which no longer matches that reality; write a
    minimal sibling `CMakeLists.txt` so the fixture stays a genuinely
    discoverable component, same as every real one. Skipped for a `rel`
    with no nested directory under `components/` (e.g. `components/
    bb_fake.c`, 2 parts) -- there's no component directory to write into,
    and that shape is the deliberate zero-nesting-depth gap fixture for
    `owner_of_path`'s fallback (kept broken on purpose, unrelated to this
    fix)."""
    parts = Path(rel).parts
    if len(parts) < 3 or parts[0] != "components":
        return
    cmake = root / "components" / parts[1] / "CMakeLists.txt"
    if not cmake.is_file():
        cmake.parent.mkdir(parents=True, exist_ok=True)
        cmake.write_text("idf_component_register()\n", encoding="utf-8")


class TestFamilyDiscovery(unittest.TestCase):
    def test_callback_slot_is_discovered(self):
        self.assertIn("callback_slot", fence_pkg.FAMILIES)

    def test_baseline_path_is_per_family_convention(self):
        path = fence_pkg.baseline_path("/repo", "callback_slot")
        self.assertEqual(path, Path("/repo/.baseline/bbtool/fence/callback_slot.json"))


class TestScanHandRolledSlot(unittest.TestCase):
    def test_finds_typedef_void0_slot(self):
        # A no-arg, void-fire hand-rolled slot (the VOID0 shape) using a
        # typedef'd callback type resolved via the header's typedef table.
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "components/bb_fake/include/bb_fake.h", (
                "#pragma once\n"
                "typedef void (*bb_fake_on_ready_cb_t)(void);\n"
                "void bb_fake_register_on_ready(bb_fake_on_ready_cb_t cb);\n"
            ))
            _write(root, "components/bb_fake/src/bb_fake.c", (
                "#include \"bb_fake.h\"\n"
                "static bb_fake_on_ready_cb_t s_on_ready_cb = NULL;\n"
                "\n"
                "void bb_fake_register_on_ready(bb_fake_on_ready_cb_t cb)\n"
                "{\n"
                "    s_on_ready_cb = cb;\n"
                "}\n"
                "\n"
                "void bb_fake_fire_ready(void)\n"
                "{\n"
                "    if (s_on_ready_cb) {\n"
                "        s_on_ready_cb();\n"
                "    }\n"
                "}\n"
            ))
            found = scan_all(str(root))
            self.assertIn(
                Marker(
                    "callback_slot",
                    "components/bb_fake/src/bb_fake.c",
                    "bb_fake:bb_fake:s_on_ready_cb",
                ),
                found,
            )

    def test_finds_raw_funcptr_ret_shape_slot(self):
        # A no-arg, value-returning-with-default slot (the RET shape) using
        # raw function-pointer declaration syntax (no typedef indirection).
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "platform/host/bb_fake/bb_fake_host.c", (
                "static bool (*s_query_hook)(void) = NULL;\n"
                "\n"
                "void bb_fake_set_query_hook(bool (*hook)(void))\n"
                "{\n"
                "    s_query_hook = hook;\n"
                "}\n"
                "\n"
                "bool bb_fake_query(void)\n"
                "{\n"
                "    return s_query_hook ? s_query_hook() : true;\n"
                "}\n"
            ))
            found = scan_all(str(root))
            self.assertIn(
                Marker(
                    "callback_slot",
                    "platform/host/bb_fake/bb_fake_host.c",
                    "bb_fake:bb_fake_host:s_query_hook",
                ),
                found,
            )

    def test_finds_with_args_void_shape_one_liner_setter(self):
        # The VOID (with-args) shape, one-line setter definition — exercises
        # the non-line-anchored assignment search (see module docstring on
        # why the setter regex cannot anchor to `^`).
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "platform/host/bb_fake/bb_fake_host.c", (
                "static void (*s_notify_hook)(uint32_t ms) = NULL;\n"
                "void bb_fake_set_notify_hook(void (*hook)(uint32_t ms)) { s_notify_hook = hook; }\n"
                "void bb_fake_notify(uint32_t ms) { if (s_notify_hook) { s_notify_hook(ms); } }\n"
            ))
            found = scan_all(str(root))
            self.assertIn(
                Marker(
                    "callback_slot",
                    "platform/host/bb_fake/bb_fake_host.c",
                    "bb_fake:bb_fake_host:s_notify_hook",
                ),
                found,
            )


class TestDoesNotMatchSanctionedMacro(unittest.TestCase):
    def test_macro_instantiation_not_counted(self):
        # BB_CALLBACK_SLOT_*(...) is a single macro-call line — no literal
        # `static ... = NULL;` declaration or hand-written setter exists in
        # the raw source text for this scanner to see.
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "platform/host/bb_fake/bb_fake_emit.c", (
                "#include \"bb_callback_slot.h\"\n"
                "typedef void (*bb_fake_on_ready_cb_t)(void);\n"
                "BB_CALLBACK_SLOT_VOID0(on_ready, bb_fake_on_ready_cb_t,\n"
                "                       bb_fake_register_on_ready, bb_fake_fire_ready)\n"
            ))
            found = scan_all(str(root))
            self.assertEqual(found, set())

    def test_canonical_bb_core_own_definition_excluded(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "components/bb_core/include/bb_callback_slot.h", (
                "#define BB_CALLBACK_SLOT_VOID0(slot, cb_type, setter, invoke) \\\n"
                "    static cb_type bb_callback_slot_##slot = NULL; \\\n"
                "    void setter(cb_type cb) \\\n"
                "    { \\\n"
                "        bb_callback_slot_##slot = cb; \\\n"
                "    } \\\n"
                "    void invoke(void) \\\n"
                "    { \\\n"
                "        cb_type cb = bb_callback_slot_##slot; \\\n"
                "        if (cb) { \\\n"
                "            cb(); \\\n"
                "        } \\\n"
                "    }\n"
            ))
            found = scan_all(str(root))
            self.assertEqual(found, set())


class TestReturnValuePlusArgsShapeExcluded(unittest.TestCase):
    """ACCEPTED LIMITATION #1: a callback that both takes arguments AND
    returns a value (e.g. an injectable allocator hook) matches neither
    BB_CALLBACK_SLOT_RET (no-arg only) nor VOID/VOID0 (never return a
    value) — out of scope, never flagged."""

    def test_arg_and_return_value_raw_funcptr_not_counted(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "platform/host/bb_fake/bb_fake_host.c", (
                "static void *(*s_malloc_fn)(size_t) = NULL;\n"
                "void bb_fake_set_malloc(void *(*fn)(size_t)) { s_malloc_fn = fn; }\n"
                "static void *fake_malloc(size_t sz) { return s_malloc_fn ? s_malloc_fn(sz) : malloc(sz); }\n"
            ))
            found = scan_all(str(root))
            self.assertEqual(found, set())

    def test_arg_and_return_value_typedef_not_counted(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "components/bb_fake/include/bb_fake.h", (
                "#pragma once\n"
                "typedef bb_err_t (*bb_fake_save_cb_t)(int req, const char *body, int len);\n"
            ))
            _write(root, "platform/espidf/bb_fake/bb_fake.c", (
                "#include \"bb_fake.h\"\n"
                "static bb_fake_save_cb_t s_save_cb = NULL;\n"
                "void bb_fake_set_save_cb(bb_fake_save_cb_t cb) { s_save_cb = cb; }\n"
                "bb_err_t bb_fake_save(int req, const char *body, int len)\n"
                "{\n"
                "    if (s_save_cb) { return s_save_cb(req, body, len); }\n"
                "    return BB_OK;\n"
                "}\n"
            ))
            found = scan_all(str(root))
            self.assertEqual(found, set())


class TestCtxCarryingTwoParamSetterExcluded(unittest.TestCase):
    """ACCEPTED LIMITATION #2: a ctx-carrying two-parameter setter
    (`set_foo_cb(cb, ctx)`) is a different, richer idiom the macro set's
    fixed one-parameter setter doesn't cover — out of scope."""

    def test_two_param_setter_not_counted(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "platform/host/bb_fake/bb_fake_host.c", (
                "static void (*s_persist_cb)(void *ctx, int val) = NULL;\n"
                "static void *s_persist_ctx = NULL;\n"
                "\n"
                "void bb_fake_set_persist_cb(void (*cb)(void *ctx, int val), void *ctx)\n"
                "{\n"
                "    s_persist_cb  = cb;\n"
                "    s_persist_ctx = ctx;\n"
                "}\n"
                "\n"
                "void bb_fake_invoke_persist(int val)\n"
                "{\n"
                "    if (s_persist_cb) {\n"
                "        s_persist_cb(s_persist_ctx, val);\n"
                "    }\n"
                "}\n"
            ))
            found = scan_all(str(root))
            self.assertEqual(found, set())


class TestTestOnlyHookExcluded(unittest.TestCase):
    """ACCEPTED LIMITATION #5: a slot whose declaration/setter sits inside
    an enclosing `#ifdef *_TESTING`/`*_TEST`/`UNIT_TEST` block is a
    test-only hook, not production composition wiring — out of scope,
    mirroring `fence/_base.py`'s `EXCLUDE_DIRS` treatment of `test/`."""

    def test_ifdef_testing_guarded_hook_not_counted(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "platform/espidf/bb_fake/bb_fake.c", (
                "#ifdef BB_FAKE_TESTING\n"
                "static void (*s_race_hook)(const char *key) = NULL;\n"
                "\n"
                "void bb_fake_test_set_race_hook(void (*hook)(const char *key))\n"
                "{\n"
                "    s_race_hook = hook;\n"
                "}\n"
                "#endif\n"
            ))
            found = scan_all(str(root))
            self.assertEqual(found, set())

    def test_ifdef_testing_guard_else_branch_still_counted(self):
        # The TRUE branch of a `#ifdef *_TESTING` is the testing region;
        # the `#else` fallback branch is ordinary production code and must
        # still be scanned normally.
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "platform/espidf/bb_fake/bb_fake.c", (
                "#ifdef BB_FAKE_TESTING\n"
                "static void (*s_race_hook)(const char *key) = NULL;\n"
                "void bb_fake_test_set_race_hook(void (*hook)(const char *key)) { s_race_hook = hook; }\n"
                "#else\n"
                "static void (*s_on_ready)(void) = NULL;\n"
                "void bb_fake_set_ready(void (*cb)(void)) { s_on_ready = cb; }\n"
                "void bb_fake_fire(void) { if (s_on_ready) { s_on_ready(); } }\n"
                "#endif\n"
            ))
            found = scan_all(str(root))
            self.assertIn(
                Marker("callback_slot", "platform/espidf/bb_fake/bb_fake.c", "bb_fake:bb_fake:s_on_ready"),
                found,
            )
            self.assertEqual(len(found), 1)

    def test_elif_branch_of_testing_ifdef_is_production_regression_guard(self):
        # Regression guard for the HIGH bug: `#elif` matched none of the
        # directive regexes, so the enclosing `#ifdef *_TESTING` frame's
        # `own_matches=True`/`in_else=False` silently persisted across the
        # `#elif` line -- a PRODUCTION slot in that branch was wrongly
        # treated as testing and dropped from the scan (never tracked,
        # never failing the fence). This is the reviewer's exact repro.
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "platform/espidf/bb_fake/bb_fake.c", (
                "#ifdef BB_FAKE_TESTING\n"
                "static void (*s_test_hook)(void) = NULL;\n"
                "void bb_fake_test_set_hook(void (*cb)(void)) { s_test_hook = cb; }\n"
                "#elif defined(BB_FAKE_ALT)\n"
                "static void (*s_prod_hook)(void) = NULL;\n"
                "void bb_fake_set_hook(void (*cb)(void)) { s_prod_hook = cb; }\n"
                "#endif\n"
            ))
            found = scan_all(str(root))
            self.assertIn(
                Marker("callback_slot", "platform/espidf/bb_fake/bb_fake.c", "bb_fake:bb_fake:s_prod_hook"),
                found,
            )

    def test_elif_branch_testing_hook_is_over_counted_not_excluded(self):
        # Pins the accepted, SAFE over-count direction: a testing hook that
        # itself lives in an `#elif defined(FOO_TESTING)` branch is not
        # recognized as testing (no per-`#elif`-branch condition
        # re-evaluation) and so is scanned normally -- over-counted
        # (seeded/frozen as if production) rather than silently dropped.
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "platform/espidf/bb_fake/bb_fake.c", (
                "#ifdef BB_FAKE_ALT\n"
                "static void (*s_on_ready)(void) = NULL;\n"
                "void bb_fake_set_ready(void (*cb)(void)) { s_on_ready = cb; }\n"
                "void bb_fake_fire(void) { if (s_on_ready) { s_on_ready(); } }\n"
                "#elif defined(BB_FAKE_TESTING)\n"
                "static void (*s_test_hook)(void) = NULL;\n"
                "void bb_fake_test_set_hook(void (*cb)(void)) { s_test_hook = cb; }\n"
                "#endif\n"
            ))
            found = scan_all(str(root))
            self.assertIn(
                Marker("callback_slot", "platform/espidf/bb_fake/bb_fake.c", "bb_fake:bb_fake:s_test_hook"),
                found,
            )


class TestCommentedOutSetterExcluded(unittest.TestCase):
    """[nit] a commented-out one-line setter must not be a latent false
    positive — the setter-assignment match is gated through
    `_base.is_noise_line`, mirroring the declaration match's own gate."""

    def test_commented_out_setter_not_counted(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "platform/host/bb_fake/bb_fake_host.c", (
                "static void (*s_on_ready)(void) = NULL;\n"
                "// void bb_fake_set_ready(void (*cb)(void)) { s_on_ready = cb; }\n"
                "void bb_fake_fire(void) { if (s_on_ready) { s_on_ready(); } }\n"
            ))
            found = scan_all(str(root))
            self.assertEqual(found, set())


class TestExcludedDirs(unittest.TestCase):
    def test_build_and_test_dirs_skipped(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            content = (
                "static void (*s_on_ready)(void) = NULL;\n"
                "void bb_fake_set_ready(void (*cb)(void)) { s_on_ready = cb; }\n"
                "void bb_fake_fire(void) { if (s_on_ready) { s_on_ready(); } }\n"
            )
            _write(root, "components/bb_fake/build/generated.c", content)
            _write(root, "components/bb_fake/test/test_fake.c", content)
            found = scan_all(str(root))
            self.assertEqual(found, set())


class TestCountsByBucket(unittest.TestCase):
    def test_bucket_labels(self):
        markers = {Marker("callback_slot", "b.c", "bb_fake:b:s_on_ready")}
        counts = counts_by_bucket(markers)
        self.assertEqual(counts, {"hand-rolled callback_slot": 1})


def _run_fence_cli(root: str, family=None, update_baseline: bool = False, seed=None) -> tuple:
    args = argparse.Namespace(root=root, family=family, update_baseline=update_baseline, seed=seed)
    stdout, stderr = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
        rc = fence_cmd.run(args)
    return rc, stdout.getvalue(), stderr.getvalue()


class TestFenceCliCallbackSlot(unittest.TestCase):
    """Exercises the generic `fence` CLI's shrink-only / net-new semantics
    against the real callback_slot family scanner, on a synthetic tree —
    proving the ratchet: seed, clean run passes, a fresh duplicate fails,
    and `--update-baseline` never blesses a net-new site."""

    def _slot_src(self, name="s_on_ready", setter="bb_fake_set_ready"):
        return (
            f"static void (*{name})(void) = NULL;\n"
            f"void {setter}(void (*cb)(void)) {{ {name} = cb; }}\n"
            f"void bb_fake_fire(void) {{ if ({name}) {{ {name}(); }} }}\n"
        )

    def test_seed_then_clean_run_passes(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "components/bb_fake/src/bb_fake.c", self._slot_src())

            rc, out, _ = _run_fence_cli(str(root), seed="callback_slot")
            self.assertEqual(rc, 0)
            self.assertIn("baseline seeded", out)

            rc2, out2, _ = _run_fence_cli(str(root), family=["callback_slot"])
            self.assertEqual(rc2, 0)
            self.assertIn("PASS", out2)

    def test_new_synthetic_slot_fails(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "components/bb_fake/src/bb_fake.c", self._slot_src())
            _run_fence_cli(str(root), seed="callback_slot")

            _write(root, "components/bb_fake2/src/bb_fake2.c", self._slot_src(
                name="s_on_other", setter="bb_fake_set_other",
            ))

            rc, out, err = _run_fence_cli(str(root), family=["callback_slot"])
            self.assertEqual(rc, 1)
            self.assertIn("new marker added", err)
            self.assertIn("s_on_other", err)

    def test_update_baseline_does_not_bless_new_slot(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            src = _write(root, "components/bb_fake/src/bb_fake.c", self._slot_src())
            _run_fence_cli(str(root), seed="callback_slot")

            # Simultaneously: remove the seeded slot AND add a new one.
            src.write_text(self._slot_src(name="s_on_other", setter="bb_fake_set_other"), encoding="utf-8")

            rc, out, _ = _run_fence_cli(str(root), family=["callback_slot"], update_baseline=True)
            self.assertEqual(rc, 0)
            self.assertIn("baseline pruned", out)
            self.assertIn("NOT added to the", out)

            baseline = fence_pkg.load_baseline(str(root), "callback_slot")
            baseline_ids = {m.id for m in baseline}
            self.assertFalse(
                any("s_on_other" in bid for bid in baseline_ids),
                "net-new slot must never be blessed",
            )

            rc2, _, err2 = _run_fence_cli(str(root), family=["callback_slot"])
            self.assertEqual(rc2, 1)
            self.assertIn("s_on_other", err2)

    def test_second_instance_in_new_dir_with_matching_stem_fails(self):
        # B1-917 repro: id = component:file-stem:name. A NEW directory
        # (different platform layer) whose component name AND filename
        # stem both match an already-baselined site, reusing the same
        # slot var name, collapses onto the identical identity
        # ("bb_fake:bb_fake:s_on_ready"). Must now FAIL, not silently
        # PASS as "0 new".
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "platform/host/bb_fake/bb_fake.c", self._slot_src())
            _run_fence_cli(str(root), seed="callback_slot")

            _write(root, "platform/espidf/bb_fake/bb_fake.c", self._slot_src())

            rc, out, err = _run_fence_cli(str(root), family=["callback_slot"])
            self.assertEqual(rc, 1, "a second occurrence reusing a baselined identity must fail")
            self.assertIn("new marker added", err)
            self.assertIn("platform/espidf/bb_fake/bb_fake.c", err)

    def test_migrated_site_prunes_cleanly(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            src = _write(root, "components/bb_fake/src/bb_fake.c", self._slot_src())
            _run_fence_cli(str(root), seed="callback_slot")

            # Migrate onto the bb_core macro: the hand-rolled slot is gone.
            src.write_text(
                "#include \"bb_callback_slot.h\"\n"
                "BB_CALLBACK_SLOT_VOID0(on_ready, bb_fake_on_ready_cb_t,\n"
                "                       bb_fake_set_ready, bb_fake_fire)\n",
                encoding="utf-8",
            )

            rc, out, _ = _run_fence_cli(str(root), family=["callback_slot"])
            self.assertEqual(rc, 0, "removing a hand-rolled slot must never fail the fence")
            self.assertIn("PASS", out)
            self.assertIn("candidate to prune from baseline", out)

            rc2, out2, _ = _run_fence_cli(str(root), family=["callback_slot"], update_baseline=True)
            self.assertEqual(rc2, 0)
            baseline = fence_pkg.load_baseline(str(root), "callback_slot")
            self.assertEqual(baseline, set())


class TestComponentOfDelegatesToOwnerOfPath(unittest.TestCase):
    """B1-1089: `callback_slot._component_of` now delegates to the
    canonical `discovery.owner_of_path`."""

    def test_component_under_components_src(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(os.path.realpath(td))
            _write(root, "components/bb_fake/src/x.c", "// x\n")
            index = build_index([str(root)])
            self.assertEqual(
                _component_of(index, "components/bb_fake/src/x.c"), "bb_fake"
            )

    def test_component_under_platform_variant(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(os.path.realpath(td))
            _write(root, "platform/espidf/bb_fake/bb_fake.c", "// x\n")
            index = build_index([str(root)])
            self.assertEqual(
                _component_of(index, "platform/espidf/bb_fake/bb_fake.c"), "bb_fake"
            )

    def test_path_outside_any_root_convention_falls_back_not_dropped(self):
        # See test_fence_clamp.py's twin test for the full WATCH FOR
        # rationale: None from owner_of_path must fall back, never
        # silently drop the marker's identity. Also asserts the fallback
        # is OBSERVABLE (WARN on stderr + bumped counter), not silent.
        with tempfile.TemporaryDirectory() as td:
            root = Path(os.path.realpath(td))
            index = build_index([str(root)])
            fence_pkg.reset_owner_fallback_count("callback_slot")
            stderr = io.StringIO()
            with contextlib.redirect_stderr(stderr):
                result = _component_of(index, "examples/floor/main.c")
            self.assertEqual(result, "examples")
            self.assertEqual(fence_pkg.owner_fallback_count("callback_slot"), 1)
            self.assertIn("WARN [fence:callback_slot]", stderr.getvalue())
            self.assertIn("examples/floor/main.c", stderr.getvalue())

    def test_relative_spelling_resolves(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(os.path.realpath(td))
            _write(root, "components/bb_fake/bb_fake.c", "// x\n")
            index = build_index([str(root)])
            self.assertEqual(
                _component_of(index, "components/bb_fake/bb_fake.c"), "bb_fake"
            )

    # NOTE (review finding 4): see test_fence_clamp.py's identical NOTE —
    # the absolute-path symlink variant here was dropped as redundant with
    # test_discovery.py's direct owner_of_path symlink coverage; production
    # never calls _component_of with an absolute path.


if __name__ == "__main__":
    unittest.main()
