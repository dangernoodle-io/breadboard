"""fence.new_component family tests: directory scanning (fires + does not
fire), family auto-discovery, the shrink-only baseline invariant, and the
grow-by-approval `--approve` CLI flow that is this family's one deliberate
departure from every other family's shrink-only semantics."""
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
from fence.new_component import scan_all, counts_by_bucket, rename_pairs  # noqa: E402
from commands import fence_cmd  # noqa: E402


def _write(root: Path, rel: str, content: str = "") -> Path:
    path = root / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")
    return path


def _mkcomponent(root: Path, name: str) -> None:
    _write(root, f"components/{name}/README.md", f"# {name}\n")


class TestFamilyDiscovery(unittest.TestCase):
    def test_new_component_is_discovered(self):
        self.assertIn("new_component", fence_pkg.FAMILIES)

    def test_baseline_path_is_per_family_convention(self):
        path = fence_pkg.baseline_path("/repo", "new_component")
        self.assertEqual(path, Path("/repo/.baseline/bbtool/fence/new_component.json"))


class TestScanNewComponent(unittest.TestCase):
    def test_finds_each_component_directory(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _mkcomponent(root, "bb_example_a")
            _mkcomponent(root, "bb_example_b")
            found = scan_all(str(root))
            self.assertEqual(
                found,
                {
                    Marker("component", "components/bb_example_a", "bb_example_a"),
                    Marker("component", "components/bb_example_b", "bb_example_b"),
                },
            )

    def test_files_directly_under_components_are_not_components(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "components/README.md", "# index\n")
            _write(root, "components/.gitkeep", "")
            found = scan_all(str(root))
            self.assertEqual(found, set())

    def test_no_components_dir_yields_empty_set(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            found = scan_all(str(root))
            self.assertEqual(found, set())

    def test_nested_files_within_a_component_do_not_add_extra_markers(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _mkcomponent(root, "bb_example")
            _write(root, "components/bb_example/src/bb_example.c", "int x;\n")
            _write(root, "components/bb_example/include/bb_example.h", "\n")
            found = scan_all(str(root))
            self.assertEqual(
                found, {Marker("component", "components/bb_example", "bb_example")}
            )


class TestRenamePairs(unittest.TestCase):
    """Unit tests for the B1-1015 rename-detection heuristic in isolation
    from the CLI plumbing."""

    def test_1to1_pair_is_dropped_as_a_rename(self):
        new = [Marker("component", "components/bb_wifi_prov", "bb_wifi_prov")]
        removed = [Marker("component", "components/bb_prov", "bb_prov")]
        got_new, got_removed = rename_pairs(new, removed)
        self.assertEqual(got_new, [])
        self.assertEqual(got_removed, [])

    def test_new_with_no_removed_is_left_untouched(self):
        new = [Marker("component", "components/bb_speculative", "bb_speculative")]
        removed = []
        got_new, got_removed = rename_pairs(new, removed)
        self.assertEqual(got_new, new)
        self.assertEqual(got_removed, removed)

    def test_removed_with_no_new_is_left_untouched(self):
        new = []
        removed = [Marker("component", "components/bb_old", "bb_old")]
        got_new, got_removed = rename_pairs(new, removed)
        self.assertEqual(got_new, new)
        self.assertEqual(got_removed, removed)

    def test_multiple_new_and_removed_is_ambiguous_and_left_untouched(self):
        new = [
            Marker("component", "components/bb_speculative_1", "bb_speculative_1"),
            Marker("component", "components/bb_speculative_2", "bb_speculative_2"),
        ]
        removed = [Marker("component", "components/bb_example_a", "bb_example_a")]
        got_new, got_removed = rename_pairs(new, removed)
        self.assertEqual(got_new, new)
        self.assertEqual(got_removed, removed)


class TestCountsByBucket(unittest.TestCase):
    def test_bucket_labels(self):
        markers = {Marker("component", "components/bb_example", "bb_example")}
        counts = counts_by_bucket(markers)
        self.assertEqual(counts, {"new component": 1})


def _run_fence_cli(root: str, family=None, update_baseline: bool = False, seed=None, approve=None) -> tuple:
    args = argparse.Namespace(
        root=root, family=family, update_baseline=update_baseline, seed=seed, approve=approve
    )
    stdout, stderr = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
        rc = fence_cmd.run(args)
    return rc, stdout.getvalue(), stderr.getvalue()


class TestFenceCliNewComponent(unittest.TestCase):
    """Exercises the generic `fence` CLI's semantics against the real
    new_component family scanner, plus the --approve grow-by-approval
    path, on a synthetic tree."""

    def test_seed_captures_all_current_components_and_passes(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _mkcomponent(root, "bb_example_a")
            _mkcomponent(root, "bb_example_b")

            rc, out, _ = _run_fence_cli(str(root), seed="new_component")
            self.assertEqual(rc, 0)
            self.assertIn("baseline seeded (2", out)

            rc2, out2, _ = _run_fence_cli(str(root), family=["new_component"])
            self.assertEqual(rc2, 0)
            self.assertIn("PASS", out2)

    def test_new_unapproved_component_fails_the_fence(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _mkcomponent(root, "bb_example_a")
            _run_fence_cli(str(root), seed="new_component")

            _mkcomponent(root, "bb_speculative")

            rc, out, err = _run_fence_cli(str(root), family=["new_component"])
            self.assertEqual(rc, 1)
            self.assertIn("new marker added", err)
            self.assertIn("bb_speculative", err)

    def test_approve_adds_exactly_that_entry_then_passes(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _mkcomponent(root, "bb_example_a")
            _run_fence_cli(str(root), seed="new_component")

            _mkcomponent(root, "bb_approved")
            _mkcomponent(root, "bb_still_unapproved")

            rc, out, _ = _run_fence_cli(str(root), approve="bb_approved")
            self.assertEqual(rc, 0)
            self.assertIn("approved components/bb_approved", out)

            baseline = fence_pkg.load_baseline(str(root), "new_component")
            baseline_ids = {m.id for m in baseline}
            self.assertIn("bb_approved", baseline_ids)
            self.assertNotIn("bb_still_unapproved", baseline_ids)

            # The still-unapproved sibling continues to fail the fence.
            rc2, out2, err2 = _run_fence_cli(str(root), family=["new_component"])
            self.assertEqual(rc2, 1)
            self.assertNotIn("bb_approved", err2)
            self.assertIn("bb_still_unapproved", err2)

            # Approving the last one clears the fence entirely.
            _run_fence_cli(str(root), approve="bb_still_unapproved")
            rc3, out3, _ = _run_fence_cli(str(root), family=["new_component"])
            self.assertEqual(rc3, 0)
            self.assertIn("PASS", out3)

    def test_approve_only_touches_new_component_baseline(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _mkcomponent(root, "bb_example_a")
            _write(root, "components/bb_scalar/src/bb_scalar.c", "bool bb_scalar_parse_bool(void) { return false; }\n")
            _run_fence_cli(str(root), seed="new_component")
            _run_fence_cli(str(root), seed="scalar_parse")

            scalar_baseline_before = fence_pkg.load_baseline(str(root), "scalar_parse")

            _mkcomponent(root, "bb_approved")
            rc, _, _ = _run_fence_cli(str(root), approve="bb_approved")
            self.assertEqual(rc, 0)

            scalar_baseline_after = fence_pkg.load_baseline(str(root), "scalar_parse")
            self.assertEqual(scalar_baseline_before, scalar_baseline_after)

    def test_approve_nonexistent_component_errors(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _mkcomponent(root, "bb_example_a")
            _run_fence_cli(str(root), seed="new_component")

            rc, out, err = _run_fence_cli(str(root), approve="bb_does_not_exist")
            self.assertEqual(rc, 1)
            self.assertIn("does not exist", err)

            baseline = fence_pkg.load_baseline(str(root), "new_component")
            baseline_ids = {m.id for m in baseline}
            self.assertNotIn("bb_does_not_exist", baseline_ids)

    def test_approve_already_approved_component_is_a_noop(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _mkcomponent(root, "bb_example_a")
            _run_fence_cli(str(root), seed="new_component")

            rc, out, _ = _run_fence_cli(str(root), approve="bb_example_a")
            self.assertEqual(rc, 0)
            self.assertIn("already approved", out)

    def test_removed_component_prunes_via_update_baseline(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _mkcomponent(root, "bb_example_a")
            _mkcomponent(root, "bb_example_b")
            _run_fence_cli(str(root), seed="new_component")

            # bb_example_b is deleted from disk (e.g. the bb_pub_wifi case).
            import shutil
            shutil.rmtree(root / "components" / "bb_example_b")

            rc, out, _ = _run_fence_cli(str(root), family=["new_component"])
            self.assertEqual(rc, 0, "a removed component must never fail the fence")
            self.assertIn("candidate to prune from baseline", out)

            rc2, out2, _ = _run_fence_cli(str(root), family=["new_component"], update_baseline=True)
            self.assertEqual(rc2, 0)
            baseline = fence_pkg.load_baseline(str(root), "new_component")
            baseline_ids = {m.id for m in baseline}
            self.assertEqual(baseline_ids, {"bb_example_a"})

    def test_update_baseline_does_not_bless_an_unapproved_new_component(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _mkcomponent(root, "bb_example_a")
            _mkcomponent(root, "bb_example_b")
            _run_fence_cli(str(root), seed="new_component")

            # Simultaneously: remove ONE seeded component AND add TWO new,
            # unapproved ones — a 2-new-for-1-removed shape is NOT the
            # unambiguous 1:1 rename pairing (B1-1015), so it stays
            # net-new + prune-candidate and the shrink-only invariant must
            # still hold.
            import shutil
            shutil.rmtree(root / "components" / "bb_example_a")
            _mkcomponent(root, "bb_speculative_1")
            _mkcomponent(root, "bb_speculative_2")

            rc, out, _ = _run_fence_cli(str(root), family=["new_component"], update_baseline=True)
            self.assertEqual(rc, 0)
            self.assertIn("baseline pruned", out)
            self.assertIn("NOT added to the", out)

            baseline = fence_pkg.load_baseline(str(root), "new_component")
            baseline_ids = {m.id for m in baseline}
            self.assertNotIn("bb_speculative_1", baseline_ids, "net-new component must never be blessed by --update-baseline")
            self.assertNotIn("bb_speculative_2", baseline_ids, "net-new component must never be blessed by --update-baseline")

            rc2, _, err2 = _run_fence_cli(str(root), family=["new_component"])
            self.assertEqual(rc2, 1)
            self.assertIn("bb_speculative_1", err2)
            self.assertIn("bb_speculative_2", err2)

    def test_directory_rename_is_identity_stable_without_approve(self):
        """B1-1015: renaming an already-approved component's directory
        (e.g. bb_prov -> bb_wifi_prov, B1-808) is a paired remove+add of
        exactly one component each — detected as an identity-stable
        rename, no --approve or baseline edit needed."""
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _mkcomponent(root, "bb_example_a")
            _mkcomponent(root, "bb_prov")
            _run_fence_cli(str(root), seed="new_component")

            import shutil
            shutil.rmtree(root / "components" / "bb_prov")
            _mkcomponent(root, "bb_wifi_prov")

            rc, out, _ = _run_fence_cli(str(root), family=["new_component"])
            self.assertEqual(rc, 0, "a paired 1:1 rename must pass without --approve")
            self.assertIn("PASS", out)
            # The heuristic firing is not a silent PASS — an INFO breadcrumb
            # names the paired old -> new component so `make fence`/CI
            # output shows a rename was collapsed.
            self.assertIn("INFO [fence:new_component]: rename detected:", out)
            self.assertIn("bb_prov", out)
            self.assertIn("bb_wifi_prov", out)

            # Zero baseline edit needed — the baseline still names the OLD
            # component; the rename keeps passing on every subsequent run
            # purely from the 1:1 pairing, no --update-baseline swap
            # required.
            baseline = fence_pkg.load_baseline(str(root), "new_component")
            baseline_ids = {m.id for m in baseline}
            self.assertIn("bb_prov", baseline_ids)
            self.assertNotIn("bb_wifi_prov", baseline_ids)

            rc2, out2, _ = _run_fence_cli(str(root), family=["new_component"])
            self.assertEqual(rc2, 0, "the rename keeps passing on repeat runs with no baseline edit")
            self.assertIn("PASS", out2)

    def test_directory_rename_update_baseline_is_a_noop_for_the_pair(self):
        """--update-baseline must not prune the renamed-away old entry (it
        is not a genuine removal) nor bless the new one (still shrink-only
        for everything outside the paired rename)."""
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _mkcomponent(root, "bb_prov")
            _run_fence_cli(str(root), seed="new_component")

            import shutil
            shutil.rmtree(root / "components" / "bb_prov")
            _mkcomponent(root, "bb_wifi_prov")

            rc, out, _ = _run_fence_cli(str(root), family=["new_component"], update_baseline=True)
            self.assertEqual(rc, 0)
            self.assertIn("baseline pruned (0 removed", out)
            self.assertNotIn("NOT added to the", out, "the paired rename is not reported as an unblessed new marker")
            # The --update-baseline path emits the same breadcrumb as
            # --check when the rename heuristic fires.
            self.assertIn("INFO [fence:new_component]: rename detected:", out)
            self.assertIn("bb_prov", out)
            self.assertIn("bb_wifi_prov", out)

            baseline = fence_pkg.load_baseline(str(root), "new_component")
            baseline_ids = {m.id for m in baseline}
            self.assertEqual(baseline_ids, {"bb_prov"})

    def test_approve_mutually_exclusive_with_seed_and_family(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _mkcomponent(root, "bb_example_a")
            _run_fence_cli(str(root), seed="new_component")
            _mkcomponent(root, "bb_new")

            rc, _, err = _run_fence_cli(str(root), approve="bb_new", family=["new_component"])
            self.assertEqual(rc, 1)
            self.assertIn("mutually exclusive", err)

    def test_approve_mutually_exclusive_with_seed(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _mkcomponent(root, "bb_example_a")
            _run_fence_cli(str(root), seed="new_component")
            _mkcomponent(root, "bb_new")
            baseline_before = fence_pkg.load_baseline(str(root), "new_component")

            rc, _, err = _run_fence_cli(str(root), approve="bb_new", seed="new_component")
            self.assertEqual(rc, 1)
            self.assertIn("mutually exclusive", err)

            baseline_after = fence_pkg.load_baseline(str(root), "new_component")
            self.assertEqual(baseline_before, baseline_after, "rejected --approve+--seed must not mutate the baseline")

    def test_approve_mutually_exclusive_with_update_baseline(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _mkcomponent(root, "bb_example_a")
            _run_fence_cli(str(root), seed="new_component")
            _mkcomponent(root, "bb_new")
            baseline_before = fence_pkg.load_baseline(str(root), "new_component")

            rc, _, err = _run_fence_cli(str(root), approve="bb_new", update_baseline=True)
            self.assertEqual(rc, 1)
            self.assertIn("mutually exclusive", err)

            baseline_after = fence_pkg.load_baseline(str(root), "new_component")
            self.assertEqual(baseline_before, baseline_after, "rejected --approve+--update-baseline must not mutate the baseline")

    def test_approve_empty_string_errors(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _mkcomponent(root, "bb_example_a")
            _run_fence_cli(str(root), seed="new_component")
            baseline_before = fence_pkg.load_baseline(str(root), "new_component")

            rc, _, err = _run_fence_cli(str(root), approve="")
            self.assertNotEqual(rc, 0, "--approve '' must error, not silently no-op into a default run")

            baseline_after = fence_pkg.load_baseline(str(root), "new_component")
            self.assertEqual(baseline_before, baseline_after, "--approve '' must not mutate the baseline")


if __name__ == "__main__":
    unittest.main()
