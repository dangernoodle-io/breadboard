"""fence engine + `fence` command tests: baseline load/save, diff, and CLI
semantics (multi-family PASS/FAIL, shrink-only --update-baseline, --seed).
"""
import argparse
import contextlib
import io
import os
import sys
import tempfile
import types
import unittest
from pathlib import Path

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "commands"))

import fence as fence_pkg  # noqa: E402
from fence import Marker  # noqa: E402
from commands import fence_cmd  # noqa: E402


def _write(root: Path, rel: str, content: str) -> Path:
    path = root / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")
    return path


class TestFamilyDiscovery(unittest.TestCase):
    def test_di_legacy_is_discovered(self):
        self.assertIn("di_legacy", fence_pkg.FAMILIES)

    def test_baseline_path_is_per_family_convention(self):
        path = fence_pkg.baseline_path("/repo", "di_legacy")
        self.assertEqual(path, Path("/repo/.baseline/bbtool/fence/di_legacy.json"))


def _fixture_module(name: str, *, include_bad: bool = True) -> types.ModuleType:
    """A synthetic family module exercising discover_scanners()/scan_all()'s
    signature + return-value filtering:
      - _scan_zero_arg: no positional args -> not collected (wrong arity)
      - _scan_multi_arg: two positional args -> not collected (wrong arity)
      - _scan_good: single-arg, returns a well-formed Set[Marker]
      - _scan_bad: single-arg (so IS collected), returns non-Marker elements
    """
    mod = types.ModuleType(name)

    def _scan_zero_arg():
        return set()

    def _scan_multi_arg(root, extra):
        return set()

    def _scan_good(root):
        return {Marker("t", "a.c", "x")}

    def _scan_bad(root):
        return {"not-a-marker"}

    mod._scan_zero_arg = _scan_zero_arg
    mod._scan_multi_arg = _scan_multi_arg
    mod._scan_good = _scan_good
    if include_bad:
        mod._scan_bad = _scan_bad
    return mod


class TestScannerHardening(unittest.TestCase):
    def test_discover_scanners_filters_by_arity(self):
        mod = _fixture_module("fence_test_fixture_arity")
        names = {fn.__name__ for fn in fence_pkg.discover_scanners(mod)}
        # Only single-arg _scan_* functions are collected; zero-arg and
        # multi-arg helpers (even if named _scan_*) are excluded.
        self.assertEqual(names, {"_scan_good", "_scan_bad"})

    def test_scan_all_runs_well_formed_scanner(self):
        mod = _fixture_module("fence_test_fixture_good", include_bad=False)
        found = fence_pkg.scan_all(mod, "/tmp")
        self.assertEqual(found, {Marker("t", "a.c", "x")})

    def test_scan_all_raises_clear_diagnostic_on_malformed_scanner_return(self):
        mod = _fixture_module("fence_test_fixture_bad")
        with self.assertRaises(fence_pkg.ScannerError) as ctx:
            fence_pkg.scan_all(mod, "/tmp")
        message = str(ctx.exception)
        self.assertIn("fence_test_fixture_bad._scan_bad", message)
        self.assertIn("non-Marker", message)

    def test_scan_all_raises_clear_diagnostic_on_scanner_exception(self):
        mod = types.ModuleType("fence_test_fixture_raises")

        def _scan_explodes(root):
            raise ValueError("boom")

        mod._scan_explodes = _scan_explodes

        with self.assertRaises(fence_pkg.ScannerError) as ctx:
            fence_pkg.scan_all(mod, "/tmp")
        message = str(ctx.exception)
        self.assertIn("fence_test_fixture_raises._scan_explodes", message)
        self.assertIn("ValueError", message)
        self.assertIn("boom", message)


class TestBaselineRoundtrip(unittest.TestCase):
    def test_write_then_load_roundtrips(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            markers = {
                Marker("BB_INIT_REGISTER", "components/bb_fake/src/bb_fake.c", "bb_fake"),
                Marker("pub_sink", "platform/host/bb_fake/bb_fake.c", "bb_pub_sink_t"),
            }
            path = fence_pkg.write_baseline(str(root), "di_legacy", markers)
            self.assertTrue(path.is_file())
            self.assertEqual(path, fence_pkg.baseline_path(str(root), "di_legacy"))
            loaded = fence_pkg.load_baseline(str(root), "di_legacy")
            self.assertEqual(loaded, markers)

    def test_missing_baseline_loads_empty(self):
        with tempfile.TemporaryDirectory() as td:
            self.assertEqual(fence_pkg.load_baseline(td, "nonexistent_family"), set())

    def test_write_is_deterministic(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            markers = {
                Marker("pub_sink", "b.c", "bb_pub_sink_t"),
                Marker("pub_sink", "a.c", "bb_pub_sink_t"),
            }
            path = fence_pkg.write_baseline(str(root), "di_legacy", markers)
            snapshot = path.read_text(encoding="utf-8")
            fence_pkg.write_baseline(str(root), "di_legacy", markers)
            self.assertEqual(path.read_text(encoding="utf-8"), snapshot)


class TestDiff(unittest.TestCase):
    def test_no_change_yields_empty_diff(self):
        markers = {Marker("pub_sink", "a.c", "bb_pub_sink_t")}
        new, removed = fence_pkg.diff(markers, markers)
        self.assertEqual(new, [])
        self.assertEqual(removed, [])

    def test_new_marker_detected(self):
        baseline = {Marker("pub_sink", "a.c", "bb_pub_sink_t")}
        current = baseline | {Marker("pub_sink", "b.c", "bb_pub_add_sink")}
        new, removed = fence_pkg.diff(current, baseline)
        self.assertEqual(new, [Marker("pub_sink", "b.c", "bb_pub_add_sink")])
        self.assertEqual(removed, [])

    def test_removed_marker_reported_not_failed(self):
        baseline = {
            Marker("pub_sink", "a.c", "bb_pub_sink_t"),
            Marker("pub_sink", "b.c", "bb_pub_add_sink"),
        }
        current = {Marker("pub_sink", "a.c", "bb_pub_sink_t")}
        new, removed = fence_pkg.diff(current, baseline)
        self.assertEqual(new, [])
        self.assertEqual(removed, [Marker("pub_sink", "b.c", "bb_pub_add_sink")])

    def test_custom_identity_fn_is_honored(self):
        # A custom identity_fn that always collapses to one bucket makes
        # every marker "the same" identity, so no diff is ever reported.
        baseline = {Marker("t", "a.c", "x")}
        current = {Marker("t", "b.c", "y")}
        new, removed = fence_pkg.diff(current, baseline, identity_fn=lambda m: ("same",))
        self.assertEqual(new, [])
        self.assertEqual(removed, [])


def _run_fence_cli(root: str, family=None, update_baseline: bool = False, seed=None) -> tuple:
    args = argparse.Namespace(root=root, family=family, update_baseline=update_baseline, seed=seed)
    stdout, stderr = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
        rc = fence_cmd.run(args)
    return rc, stdout.getvalue(), stderr.getvalue()


class TestFenceCliDiLegacy(unittest.TestCase):
    """Exercises the `fence` command end-to-end against the real di_legacy
    family scanners, on a synthetic tree standing in for a brand-new
    family with no baseline yet (proving --seed / --update-baseline
    semantics independent of the real committed baseline)."""

    def test_seed_then_clean_run_passes(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "components/bb_fake/src/bb_fake.c", "BB_INIT_REGISTER(bb_fake, bb_fake_init);\n")

            rc, out, _ = _run_fence_cli(str(root), seed="di_legacy")
            self.assertEqual(rc, 0)
            self.assertIn("baseline seeded", out)
            self.assertTrue(fence_pkg.baseline_path(str(root), "di_legacy").is_file())

            rc2, out2, _ = _run_fence_cli(str(root), family=["di_legacy"])
            self.assertEqual(rc2, 0)
            self.assertIn("PASS", out2)

    def test_seed_refuses_when_baseline_already_exists(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "components/bb_fake/src/bb_fake.c", "BB_INIT_REGISTER(bb_fake, bb_fake_init);\n")
            _run_fence_cli(str(root), seed="di_legacy")

            rc, out, err = _run_fence_cli(str(root), seed="di_legacy")
            self.assertEqual(rc, 1)
            self.assertIn("already exists", err)

    def test_new_marker_fails_with_actionable_message(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "components/bb_fake/src/bb_fake.c", "BB_INIT_REGISTER(bb_fake, bb_fake_init);\n")
            _run_fence_cli(str(root), seed="di_legacy")

            _write(root, "components/bb_fake/src/bb_fake.c", (
                "BB_INIT_REGISTER(bb_fake, bb_fake_init);\n"
                "BB_INIT_REGISTER(bb_fake_new, bb_fake_new_init);\n"
            ))

            rc, out, err = _run_fence_cli(str(root), family=["di_legacy"])
            self.assertEqual(rc, 1)
            self.assertIn("new marker added", err)
            self.assertIn("bb_fake_new", err)

    def test_rename_of_marker_file_does_not_fail(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            old_path = _write(root, "components/bb_fake/src/bb_fake_old.c", (
                "BB_INIT_REGISTER(bb_fake, bb_fake_init);\n"
            ))
            _run_fence_cli(str(root), seed="di_legacy")

            old_path.unlink()
            _write(root, "components/bb_fake/src/bb_fake_renamed.c", (
                "BB_INIT_REGISTER(bb_fake, bb_fake_init);\n"
            ))

            rc, out, _ = _run_fence_cli(str(root), family=["di_legacy"])
            self.assertEqual(rc, 0, "a pure file rename must never fail the fence")
            self.assertIn("PASS", out)
            self.assertNotIn("candidate to prune", out)

    def test_removed_marker_passes_and_reports_prune_candidate(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            src = _write(root, "components/bb_fake/src/bb_fake.c", (
                "BB_INIT_REGISTER(bb_fake, bb_fake_init);\n"
                "BB_INIT_REGISTER(bb_fake_gone, bb_fake_gone_init);\n"
            ))
            _run_fence_cli(str(root), seed="di_legacy")

            src.write_text("BB_INIT_REGISTER(bb_fake, bb_fake_init);\n", encoding="utf-8")

            rc, out, _ = _run_fence_cli(str(root), family=["di_legacy"])
            self.assertEqual(rc, 0, "removals must never fail the fence")
            self.assertIn("PASS", out)
            self.assertIn("candidate to prune from baseline", out)
            self.assertIn("bb_fake_gone", out)

    def test_update_baseline_prunes_removed_but_never_blesses_net_new(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            src = _write(root, "components/bb_fake/src/bb_fake.c", (
                "BB_INIT_REGISTER(bb_fake, bb_fake_init);\n"
                "BB_INIT_REGISTER(bb_fake_gone, bb_fake_gone_init);\n"
            ))
            _run_fence_cli(str(root), seed="di_legacy")

            # Simultaneously: remove one marker (legitimate shrink) AND add
            # a brand-new one (must never be silently blessed).
            src.write_text((
                "BB_INIT_REGISTER(bb_fake, bb_fake_init);\n"
                "BB_INIT_REGISTER(bb_fake_new_dup, bb_fake_new_dup_init);\n"
            ), encoding="utf-8")

            rc, out, _ = _run_fence_cli(str(root), family=["di_legacy"], update_baseline=True)
            self.assertEqual(rc, 0)
            self.assertIn("baseline pruned", out)
            self.assertIn("NOT added to the", out)

            baseline = fence_pkg.load_baseline(str(root), "di_legacy")
            baseline_ids = {m.id for m in baseline}
            self.assertIn("bb_fake", baseline_ids)
            self.assertNotIn("bb_fake_gone", baseline_ids, "removed marker must be pruned")
            self.assertNotIn("bb_fake_new_dup", baseline_ids, "net-new marker must never be blessed")

            # The net-new marker is still unfenced -> a normal run still fails.
            rc2, _, err2 = _run_fence_cli(str(root), family=["di_legacy"])
            self.assertEqual(rc2, 1)
            self.assertIn("bb_fake_new_dup", err2)

    def test_update_baseline_without_seed_errors(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "components/bb_fake/src/bb_fake.c", "BB_INIT_REGISTER(bb_fake, bb_fake_init);\n")

            rc, out, err = _run_fence_cli(str(root), family=["di_legacy"], update_baseline=True)
            self.assertEqual(rc, 1)
            self.assertIn("--seed", err)

    def test_seed_and_update_baseline_mutually_exclusive(self):
        with tempfile.TemporaryDirectory() as td:
            rc, out, err = _run_fence_cli(td, seed="di_legacy", update_baseline=True)
            self.assertEqual(rc, 1)
            self.assertIn("mutually exclusive", err)

    def test_seed_and_family_mutually_exclusive(self):
        with tempfile.TemporaryDirectory() as td:
            rc, out, err = _run_fence_cli(td, seed="di_legacy", family=["di_legacy"])
            self.assertEqual(rc, 1)
            self.assertIn("mutually exclusive", err)
            self.assertFalse(fence_pkg.baseline_path(td, "di_legacy").is_file())

    def test_unknown_family_errors(self):
        with tempfile.TemporaryDirectory() as td:
            rc, out, err = _run_fence_cli(td, family=["nonexistent_family"])
            self.assertEqual(rc, 1)
            self.assertIn("unknown family", err)

    def test_default_family_list_runs_all_discovered_families(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "components/bb_fake/src/bb_fake.c", "BB_INIT_REGISTER(bb_fake, bb_fake_init);\n")
            # Seed EVERY discovered family (not just di_legacy) so this
            # synthetic tree — which is unaware of any family's specific
            # markers — starts clean against the default "all families"
            # run below; e.g. new_component sees components/bb_fake as a
            # net-new component unless it too is seeded first.
            for name in sorted(fence_pkg.FAMILIES):
                _run_fence_cli(str(root), seed=name)

            rc, out, _ = _run_fence_cli(str(root))
            self.assertEqual(rc, 0)
            self.assertIn("di_legacy", out)


class TestAddArguments(unittest.TestCase):
    def test_parses_flags(self):
        parser = argparse.ArgumentParser()
        fence_cmd.add_arguments(parser)
        ns = parser.parse_args(["--update-baseline", "--family", "di_legacy", "--family", "other"])
        self.assertTrue(ns.update_baseline)
        self.assertEqual(ns.family, ["di_legacy", "other"])
        ns2 = parser.parse_args(["--seed", "di_legacy"])
        self.assertEqual(ns2.seed, "di_legacy")


if __name__ == "__main__":
    unittest.main()
