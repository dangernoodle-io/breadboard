"""fence.sat_sub family tests: one-sided saturating-subtract idiom
scanning (fires + does not fire), family auto-discovery, and the
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
from fence.sat_sub import scan_all, counts_by_bucket  # noqa: E402
from commands import fence_cmd  # noqa: E402


def _write(root: Path, rel: str, content: str) -> Path:
    path = root / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")
    return path


class TestFamilyDiscovery(unittest.TestCase):
    def test_sat_sub_is_discovered(self):
        self.assertIn("sat_sub", fence_pkg.FAMILIES)

    def test_baseline_path_is_per_family_convention(self):
        path = fence_pkg.baseline_path("/repo", "sat_sub")
        self.assertEqual(path, Path("/repo/.baseline/bbtool/fence/sat_sub.json"))


class TestScanSatSubForward(unittest.TestCase):
    def test_finds_ge_ternary(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "platform/host/bb_queue/bb_queue.c", (
                "static void sub(size_t *bytes_used, size_t len)\n"
                "{\n"
                "    *bytes_used = (*bytes_used >= len) ? (*bytes_used - len) : 0;\n"
                "}\n"
            ))
            found = scan_all(str(root))
            self.assertIn(
                Marker("sat_sub", "platform/host/bb_queue/bb_queue.c",
                       "bb_queue:sub:*bytes_used"),
                found,
            )

    def test_finds_gt_ternary_with_cast(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "platform/host/bb_fake/bb_fake.c", (
                "static size_t used(size_t total, size_t free_b)\n"
                "{\n"
                "    return (total > free_b) ? (size_t)(total - free_b) : 0;\n"
                "}\n"
            ))
            found = scan_all(str(root))
            self.assertIn(
                Marker("sat_sub", "platform/host/bb_fake/bb_fake.c", "bb_fake:used:total"),
                found,
            )

    def test_finds_literal_bound(self):
        # bb_mdns-style: the bound is a numeric literal, not a variable.
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "platform/host/bb_fake/bb_fake.c", (
                "static void build(char *out, size_t out_size)\n"
                "{\n"
                "    const int base_max = (out_size > 6) ? (int)(out_size - 6) : 0;\n"
                "}\n"
            ))
            found = scan_all(str(root))
            self.assertIn(
                Marker("sat_sub", "platform/host/bb_fake/bb_fake.c", "bb_fake:build:out_size"),
                found,
            )


class TestScanSatSubReversed(unittest.TestCase):
    def test_finds_lt_reversed_ternary(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "platform/host/bb_fake/bb_fake.c", (
                "static int delta(int a, int b)\n"
                "{\n"
                "    return (a < b) ? 0 : (a - b);\n"
                "}\n"
            ))
            found = scan_all(str(root))
            self.assertIn(
                Marker("sat_sub", "platform/host/bb_fake/bb_fake.c", "bb_fake:delta:a"),
                found,
            )

    def test_finds_le_reversed_ternary(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "platform/host/bb_fake/bb_fake.c", (
                "static int delta(int a, int b)\n"
                "{\n"
                "    return a <= b ? 0 : a - b;\n"
                "}\n"
            ))
            found = scan_all(str(root))
            self.assertIn(
                Marker("sat_sub", "platform/host/bb_fake/bb_fake.c", "bb_fake:delta:a"),
                found,
            )


class TestScanSatSubDecrementByOne(unittest.TestCase):
    def test_finds_decrement_by_one_variant(self):
        # bb_storage_nvs-style: bound is literal 0, amount subtracted is
        # literal 1 — the "strip the NUL terminator, floor at 0" idiom.
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "platform/host/bb_fake/bb_fake.c", (
                "static size_t str_len_of(int probed)\n"
                "{\n"
                "    return (probed > 0) ? probed - 1 : 0;\n"
                "}\n"
            ))
            found = scan_all(str(root))
            self.assertIn(
                Marker("sat_sub", "platform/host/bb_fake/bb_fake.c", "bb_fake:str_len_of:probed"),
                found,
            )

    def test_mismatched_bound_and_amount_does_not_fire(self):
        # Not a saturating-subtract idiom at all: the ternary's true branch
        # subtracts an amount unrelated to both the guard bound and 1.
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "platform/host/bb_fake/bb_fake.c", (
                "static int f(int a)\n"
                "{\n"
                "    return (a > 0) ? a - 5 : 0;\n"
                "}\n"
            ))
            found = scan_all(str(root))
            self.assertEqual(found, set())


class TestScanSatSubDelta(unittest.TestCase):
    """The post-hoc delta-then-clamp variant: `X = <expr with a
    subtraction>; if (X < 0) X = 0;` (single-line or braced)."""

    def test_finds_braced_form(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "components/bb_fake/src/bb_fake.c", (
                "static bool should_log(int64_t now_us, int64_t last_us)\n"
                "{\n"
                "    int64_t elapsed_us = now_us - last_us;\n"
                "    if (elapsed_us < 0) {\n"
                "        elapsed_us = 0;\n"
                "    }\n"
                "    return elapsed_us > 0;\n"
                "}\n"
            ))
            found = scan_all(str(root))
            self.assertIn(
                Marker("sat_sub", "components/bb_fake/src/bb_fake.c",
                       "bb_fake:should_log:elapsed_us"),
                found,
            )

    def test_finds_single_line_form(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "platform/host/bb_fake/bb_fake.c", (
                "static int compute(int entry_len, int topic_len)\n"
                "{\n"
                "    int payload_len = (int)(entry_len - topic_len - 1);\n"
                "    if (payload_len < 0) payload_len = 0;\n"
                "    return payload_len;\n"
                "}\n"
            ))
            found = scan_all(str(root))
            self.assertIn(
                Marker("sat_sub", "platform/host/bb_fake/bb_fake.c",
                       "bb_fake:compute:payload_len"),
                found,
            )

    def test_bare_negative_error_guard_does_not_fire(self):
        # The immediately preceding line is NOT a subtraction assignment
        # to the guarded var (it's a parameter, untouched) — this is the
        # ordinary "reject a negative error code" guard, not a
        # delta-then-clamp, and must never match.
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "platform/host/bb_fake/bb_fake.c", (
                "static bool f(int n)\n"
                "{\n"
                "    if (n < 0) return false;\n"
                "    return true;\n"
                "}\n"
            ))
            found = scan_all(str(root))
            self.assertEqual(found, set())

    def test_non_subtraction_prior_assignment_does_not_fire(self):
        # Prior line IS an assignment to the guarded var, but its RHS has
        # no subtraction (a plain negative literal / unrelated value) —
        # still not the delta-then-clamp idiom.
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "platform/host/bb_fake/bb_fake.c", (
                "static int f(int flag)\n"
                "{\n"
                "    int n = flag ? -1 : 5;\n"
                "    if (n < 0) n = 0;\n"
                "    return n;\n"
                "}\n"
            ))
            found = scan_all(str(root))
            self.assertEqual(found, set())

    def test_two_sided_clamp_after_unrelated_assignment_does_not_fire(self):
        # A genuine two-sided if-pair clamp (clamp.py's job) whose
        # preceding line is NOT a subtraction-to-the-same-var assignment
        # must not be picked up by the delta scanner.
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "platform/host/bb_fake/bb_fake.c", (
                "static void set_pct(int input)\n"
                "{\n"
                "    int pct = input;\n"
                "    if (pct < 0) pct = 0;\n"
                "    if (pct > 100) pct = 100;\n"
                "}\n"
            ))
            found = scan_all(str(root))
            self.assertEqual(found, set())


class TestScanSatSubNegatives(unittest.TestCase):
    def test_two_sided_clamp_does_not_fire(self):
        # This is clamp.py's job, not sat_sub's — both directions bounded.
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "platform/host/bb_fake/bb_fake.c", (
                "static int clampish(int x, int lo, int hi)\n"
                "{\n"
                "    return x < lo ? lo : (x > hi ? hi : x);\n"
                "}\n"
            ))
            found = scan_all(str(root))
            self.assertEqual(found, set())

    def test_if_pair_two_sided_clamp_does_not_fire(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "platform/host/bb_fake/bb_fake.c", (
                "static void set_pct(int pct)\n"
                "{\n"
                "    if (pct < 0) pct = 0;\n"
                "    if (pct > 100) pct = 100;\n"
                "}\n"
            ))
            found = scan_all(str(root))
            self.assertEqual(found, set())

    def test_bounded_chunk_loop_does_not_fire(self):
        # `remaining -= chunk` where `chunk` was already computed as
        # min(remaining, N) — an ordinary decrement of an already-bounded
        # quantity, not a reimplementation of a saturating-subtract guard.
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "platform/host/bb_fake/bb_fake.c", (
                "static void drain(size_t remaining)\n"
                "{\n"
                "    while (remaining > 0) {\n"
                "        size_t chunk = (remaining < 64) ? remaining : 64;\n"
                "        remaining -= chunk;\n"
                "    }\n"
                "}\n"
            ))
            found = scan_all(str(root))
            self.assertEqual(found, set())

    def test_unrelated_error_guard_does_not_fire(self):
        # `if (n < 0) return false;` — a signed error-code guard, not a
        # saturating subtract (no subtraction anywhere near it).
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "platform/host/bb_fake/bb_fake.c", (
                "static bool f(int n)\n"
                "{\n"
                "    if (n < 0) return false;\n"
                "    return true;\n"
                "}\n"
            ))
            found = scan_all(str(root))
            self.assertEqual(found, set())

    def test_comment_reference_not_counted(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "platform/host/bb_fake/bb_fake.c", (
                "static void sub(size_t *bytes_used, size_t len)\n"
                "{\n"
                "    // *bytes_used = (*bytes_used >= len) ? (*bytes_used - len) : 0;\n"
                "}\n"
            ))
            found = scan_all(str(root))
            self.assertEqual(found, set())

    def test_pointer_deref_statement_not_mistaken_for_comment(self):
        # Regression guard for the noise-line false-positive this family's
        # scanner must avoid: a stripped line starting with `*name` (an
        # ordinary pointer-dereference statement, this codebase's
        # prevailing no-space deref style) must NOT be treated as a `/*
        # ... */` doxygen-comment continuation line — only genuine
        # comment-continuation shapes (`* text`, `*/`, `**...`) are noise.
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "platform/host/bb_fake/bb_fake.c", (
                "static void sub(size_t *bytes_used, size_t len)\n"
                "{\n"
                "    *bytes_used = (*bytes_used >= len) ? (*bytes_used - len) : 0;\n"
                "}\n"
            ))
            found = scan_all(str(root))
            self.assertNotEqual(found, set())

    def test_canonical_bb_num_excluded(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "platform/host/bb_num/bb_num.c", (
                "static size_t bb_num_sat_sub_example(size_t a, size_t b)\n"
                "{\n"
                "    return (a > b) ? (a - b) : 0;\n"
                "}\n"
            ))
            found = scan_all(str(root))
            self.assertEqual(found, set())


class TestIdentityRenameStability(unittest.TestCase):
    def test_id_unchanged_when_lines_inserted_above_marker(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            src = _write(root, "platform/host/bb_fake/bb_fake.c", (
                "static size_t used(size_t total, size_t free_b)\n"
                "{\n"
                "    return (total > free_b) ? (total - free_b) : 0;\n"
                "}\n"
            ))
            before = scan_all(str(root))
            before_id = next(m.id for m in before if m.type == "sat_sub")

            src.write_text(
                "// unrelated comment\n"
                "\n"
                "static size_t used(size_t total, size_t free_b)\n"
                "{\n"
                "    return (total > free_b) ? (total - free_b) : 0;\n"
                "}\n",
                encoding="utf-8",
            )
            after = scan_all(str(root))
            after_id = next(m.id for m in after if m.type == "sat_sub")

            self.assertEqual(before_id, after_id)


class TestExcludedDirs(unittest.TestCase):
    def test_build_and_test_dirs_skipped(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "components/bb_fake/build/generated.c", (
                "static size_t used(size_t total, size_t free_b)\n"
                "{\n"
                "    return (total > free_b) ? (total - free_b) : 0;\n"
                "}\n"
            ))
            _write(root, "components/bb_fake/test/test_fake.c", (
                "static size_t used(size_t total, size_t free_b)\n"
                "{\n"
                "    return (total > free_b) ? (total - free_b) : 0;\n"
                "}\n"
            ))
            found = scan_all(str(root))
            self.assertEqual(found, set())


class TestCountsByBucket(unittest.TestCase):
    def test_bucket_labels(self):
        markers = {Marker("sat_sub", "a.c", "bb_fake:f:x")}
        counts = counts_by_bucket(markers)
        self.assertEqual(counts, {"hand-rolled saturating-subtract": 1})


def _run_fence_cli(root: str, family=None, update_baseline: bool = False, seed=None) -> tuple:
    args = argparse.Namespace(root=root, family=family, update_baseline=update_baseline, seed=seed)
    stdout, stderr = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
        rc = fence_cmd.run(args)
    return rc, stdout.getvalue(), stderr.getvalue()


class TestFenceCliSatSub(unittest.TestCase):
    """Exercises the generic `fence` CLI's shrink-only / net-new semantics
    against the real sat_sub family scanner, on a synthetic tree."""

    def _sat_sub_src(self):
        return (
            "static size_t used(size_t total, size_t free_b)\n"
            "{\n"
            "    return (total > free_b) ? (total - free_b) : 0;\n"
            "}\n"
        )

    def test_seed_then_clean_run_passes(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "platform/host/bb_fake/bb_fake.c", self._sat_sub_src())

            rc, out, _ = _run_fence_cli(str(root), seed="sat_sub")
            self.assertEqual(rc, 0)
            self.assertIn("baseline seeded", out)

            rc2, out2, _ = _run_fence_cli(str(root), family=["sat_sub"])
            self.assertEqual(rc2, 0)
            self.assertIn("PASS", out2)

    def test_new_synthetic_site_fails(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "platform/host/bb_fake/bb_fake.c", self._sat_sub_src())
            _run_fence_cli(str(root), seed="sat_sub")

            _write(root, "platform/host/bb_fake/bb_fake_new.c", (
                "static size_t remaining_of(size_t got, size_t want)\n"
                "{\n"
                "    return (got >= want) ? (got - want) : 0;\n"
                "}\n"
            ))

            rc, out, err = _run_fence_cli(str(root), family=["sat_sub"])
            self.assertEqual(rc, 1)
            self.assertIn("new marker added", err)
            self.assertIn("remaining_of", err)

    def test_update_baseline_does_not_bless_new_site(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            src = _write(root, "platform/host/bb_fake/bb_fake.c", self._sat_sub_src())
            _run_fence_cli(str(root), seed="sat_sub")

            # Simultaneously: remove the seeded site AND add a new one.
            src.write_text(
                "static size_t remaining_of(size_t got, size_t want)\n"
                "{\n"
                "    return (got >= want) ? (got - want) : 0;\n"
                "}\n",
                encoding="utf-8",
            )

            rc, out, _ = _run_fence_cli(str(root), family=["sat_sub"], update_baseline=True)
            self.assertEqual(rc, 0)
            self.assertIn("baseline pruned", out)
            self.assertIn("NOT added to the", out)

            baseline = fence_pkg.load_baseline(str(root), "sat_sub")
            baseline_ids = {m.id for m in baseline}
            self.assertNotIn(
                "bb_fake:remaining_of:got", baseline_ids,
                "net-new sat_sub site must never be blessed",
            )

            rc2, _, err2 = _run_fence_cli(str(root), family=["sat_sub"])
            self.assertEqual(rc2, 1)
            self.assertIn("remaining_of", err2)

    def test_second_instance_reusing_baselined_function_name_fails(self):
        # B1-917 repro: a NEW file, same component dir, reusing the exact
        # same enclosing symbol + guarded var text ("bb_fake:used:total")
        # -> collapses onto the already-baselined identity. Must now FAIL,
        # not silently PASS.
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "platform/host/bb_fake/bb_fake.c", self._sat_sub_src())
            _run_fence_cli(str(root), seed="sat_sub")

            _write(root, "platform/host/bb_fake/bb_fake_other.c", self._sat_sub_src())

            rc, out, err = _run_fence_cli(str(root), family=["sat_sub"])
            self.assertEqual(rc, 1, "a second occurrence reusing a baselined identity must fail")
            self.assertIn("new marker added", err)
            self.assertIn("bb_fake_other.c", err)

    def test_migrated_site_prunes_cleanly(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            src = _write(root, "platform/host/bb_fake/bb_fake.c", self._sat_sub_src())
            _run_fence_cli(str(root), seed="sat_sub")

            # Migrate onto a (hypothetical, future) shared helper: the
            # hand-rolled ternary is gone.
            src.write_text(
                "static size_t used(size_t total, size_t free_b)\n"
                "{\n"
                "    return bb_num_sat_sub(total, free_b);\n"
                "}\n",
                encoding="utf-8",
            )

            rc, out, _ = _run_fence_cli(str(root), family=["sat_sub"])
            self.assertEqual(rc, 0, "removing a hand-rolled sat_sub site must never fail the fence")
            self.assertIn("PASS", out)
            self.assertIn("candidate to prune from baseline", out)

            rc2, out2, _ = _run_fence_cli(str(root), family=["sat_sub"], update_baseline=True)
            self.assertEqual(rc2, 0)
            baseline = fence_pkg.load_baseline(str(root), "sat_sub")
            self.assertEqual(baseline, set())


if __name__ == "__main__":
    unittest.main()
