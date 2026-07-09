"""fence.clamp family tests: two-sided clamp idiom scanning (fires + does
not fire), family auto-discovery, and the shrink-only baseline semantics
(via the generic `fence` CLI) applied to this concrete family."""
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
from fence.clamp import scan_all, counts_by_bucket, _enclosing_symbol  # noqa: E402
from commands import fence_cmd  # noqa: E402


def _write(root: Path, rel: str, content: str) -> Path:
    path = root / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")
    return path


class TestFamilyDiscovery(unittest.TestCase):
    def test_clamp_is_discovered(self):
        self.assertIn("clamp", fence_pkg.FAMILIES)

    def test_baseline_path_is_per_family_convention(self):
        path = fence_pkg.baseline_path("/repo", "clamp")
        self.assertEqual(path, Path("/repo/.baseline/bbtool/fence/clamp.json"))


class TestScanClampIfPair(unittest.TestCase):
    def test_finds_if_pair_clamp(self):
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
            self.assertIn(
                Marker("clamp", "platform/host/bb_fake/bb_fake.c", "bb_fake:set_pct:pct"),
                found,
            )

    def test_finds_if_pair_clamp_ge_le_variant(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "platform/host/bb_fake/bb_fake.c", (
                "static void set_ms(int ms)\n"
                "{\n"
                "    if (ms >= 1000) ms = 1000;\n"
                "    if (ms <= 0) ms = 0;\n"
                "}\n"
            ))
            found = scan_all(str(root))
            self.assertIn(
                Marker("clamp", "platform/host/bb_fake/bb_fake.c", "bb_fake:set_ms:ms"),
                found,
            )

    def test_same_direction_pair_does_not_fire(self):
        # Both ifs bound the SAME direction — not a two-sided clamp.
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "platform/host/bb_fake/bb_fake.c", (
                "static void set_pct(int pct)\n"
                "{\n"
                "    if (pct < 0) pct = 0;\n"
                "    if (pct < -10) pct = -10;\n"
                "}\n"
            ))
            found = scan_all(str(root))
            self.assertEqual(found, set())

    def test_different_variable_pair_does_not_fire(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "platform/host/bb_fake/bb_fake.c", (
                "static void extend(int lo, int hi, int s, int e)\n"
                "{\n"
                "    if (s < lo) lo = s;\n"
                "    if (e > hi) hi = e;\n"
                "}\n"
            ))
            found = scan_all(str(root))
            self.assertEqual(found, set())

    def test_one_sided_saturating_subtract_does_not_fire(self):
        # bb_ring-style underflow-clamp-at-0: single bound, ternary, not a
        # two-sided clamp.
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "platform/host/bb_ring/bb_ring.c", (
                "static void sub(size_t *bytes_used, size_t len)\n"
                "{\n"
                "    *bytes_used = (*bytes_used >= len) ? (*bytes_used - len) : 0;\n"
                "}\n"
            ))
            found = scan_all(str(root))
            self.assertEqual(found, set())

    def test_one_sided_if_only_does_not_fire(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "platform/host/bb_fake/bb_fake.c", (
                "static void clamp_core(int core, int num_cores)\n"
                "{\n"
                "    if (core >= num_cores) core = -1;\n"
                "}\n"
            ))
            found = scan_all(str(root))
            self.assertEqual(found, set())

    def test_canonical_bb_num_impl_excluded(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "platform/host/bb_num/bb_num.c", (
                "int32_t bb_clampi(int32_t x, int32_t lo, int32_t hi)\n"
                "{\n"
                "    if (x < lo) x = lo;\n"
                "    if (x > hi) x = hi;\n"
                "    return x;\n"
                "}\n"
            ))
            found = scan_all(str(root))
            self.assertEqual(found, set())

    def test_comment_reference_not_counted(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "platform/host/bb_fake/bb_fake.c", (
                "static void set_pct(int pct)\n"
                "{\n"
                "    // if (pct < 0) pct = 0;\n"
                "    // if (pct > 100) pct = 100;\n"
                "}\n"
            ))
            found = scan_all(str(root))
            self.assertEqual(found, set())


class TestScanClampTernary(unittest.TestCase):
    def test_finds_nested_ternary_clamp(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "platform/host/bb_fake/bb_fake.c", (
                "static int clampish(int x, int lo, int hi)\n"
                "{\n"
                "    return x < lo ? lo : (x > hi ? hi : x);\n"
                "}\n"
            ))
            found = scan_all(str(root))
            self.assertIn(
                Marker("clamp", "platform/host/bb_fake/bb_fake.c", "bb_fake:clampish:x"),
                found,
            )

    def test_same_direction_ternary_does_not_fire(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "platform/host/bb_fake/bb_fake.c", (
                "static int f(int x, int lo, int hi)\n"
                "{\n"
                "    return x < lo ? lo : (x < hi ? hi : x);\n"
                "}\n"
            ))
            found = scan_all(str(root))
            self.assertEqual(found, set())


class TestScanClampMinMax(unittest.TestCase):
    def test_finds_max_min_nesting(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "platform/host/bb_fake/bb_fake.c", (
                "static int clampish(int x, int lo, int hi)\n"
                "{\n"
                "    return MAX(lo, MIN(hi, x));\n"
                "}\n"
            ))
            found = scan_all(str(root))
            self.assertIn(
                Marker("clamp", "platform/host/bb_fake/bb_fake.c", "bb_fake:clampish:x"),
                found,
            )

    def test_finds_fmaxf_fminf_nesting(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "platform/host/bb_fake/bb_fake.cpp", (
                "static float clampish(float x, float lo, float hi)\n"
                "{\n"
                "    return fmaxf(lo, fminf(hi, x));\n"
                "}\n"
            ))
            found = scan_all(str(root))
            self.assertIn(
                Marker("clamp", "platform/host/bb_fake/bb_fake.cpp", "bb_fake:clampish:x"),
                found,
            )

    def test_finds_std_max_min_nesting(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "platform/host/bb_fake/bb_fake.cpp", (
                "static int clampish(int x, int lo, int hi)\n"
                "{\n"
                "    return std::max(lo, std::min(hi, x));\n"
                "}\n"
            ))
            found = scan_all(str(root))
            self.assertIn(
                Marker("clamp", "platform/host/bb_fake/bb_fake.cpp", "bb_fake:clampish:x"),
                found,
            )

    def test_bare_min_without_max_does_not_fire(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "platform/host/bb_fake/bb_fake.c", (
                "static size_t f(size_t pos, size_t len)\n"
                "{\n"
                "    return MIN(pos, len);\n"
                "}\n"
            ))
            found = scan_all(str(root))
            self.assertEqual(found, set())


class TestCanonicalExclusion(unittest.TestCase):
    def test_bb_num_header_excluded(self):
        # components/bb_num/ (the public header) can carry clamp-shaped
        # doc-comment examples in the future; it must never self-fire, same
        # as the canonical platform/host/bb_num/ implementation.
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "components/bb_num/include/bb_num.h", (
                "// Example: if (x < lo) x = lo;\n"
                "// Example: if (x > hi) x = hi;\n"
                "int32_t bb_clampi(int32_t x, int32_t lo, int32_t hi)\n"
                "{\n"
                "    if (x < lo) x = lo;\n"
                "    if (x > hi) x = hi;\n"
                "    return x;\n"
                "}\n"
            ))
            found = scan_all(str(root))
            self.assertEqual(found, set())


class TestEnclosingSymbolKAndR(unittest.TestCase):
    def test_split_return_type_resolves_enclosing_symbol(self):
        # K&R / split-signature style: the return type sits on its own
        # line above a bare `name(...)` signature line. _ENCLOSING_FN_RE
        # alone can't see a return type here, so the bare-signature
        # fallback must resolve the real function name, not "?".
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "platform/host/bb_fake/bb_fake.c", (
                "int32_t\n"
                "bb_clampi_like(int32_t x, int32_t lo, int32_t hi)\n"
                "{\n"
                "    if (x < lo) x = lo;\n"
                "    if (x > hi) x = hi;\n"
                "    return x;\n"
                "}\n"
            ))
            found = scan_all(str(root))
            self.assertIn(
                Marker("clamp", "platform/host/bb_fake/bb_fake.c", "bb_fake:bb_clampi_like:x"),
                found,
            )
            self.assertNotIn(
                Marker("clamp", "platform/host/bb_fake/bb_fake.c", "bb_fake:?:x"),
                found,
            )

    def test_preceding_semicolon_terminated_call_not_mistaken_for_kr_signature(self):
        # A column-0 call/macro-invocation statement immediately above a
        # bare K&R signature (this codebase's convention: always
        # terminated with `;` on that line) must never itself be resolved
        # as the enclosing symbol — the backward scan must skip it and
        # keep going down to the real bare signature line.
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "platform/host/bb_fake/bb_fake.c", (
                "BB_SOME_MACRO(bb_fake, bb_fake_init);\n"
                "int32_t\n"
                "bb_clampi_like(int32_t x, int32_t lo, int32_t hi)\n"
                "{\n"
                "    if (x < lo) x = lo;\n"
                "    if (x > hi) x = hi;\n"
                "    return x;\n"
                "}\n"
            ))
            found = scan_all(str(root))
            self.assertIn(
                Marker("clamp", "platform/host/bb_fake/bb_fake.c", "bb_fake:bb_clampi_like:x"),
                found,
            )
            self.assertNotIn(
                Marker("clamp", "platform/host/bb_fake/bb_fake.c", "bb_fake:BB_SOME_MACRO:x"),
                found,
            )


class TestEnclosingSymbolUnit(unittest.TestCase):
    def test_semicolon_terminated_bare_call_line_is_skipped(self):
        # Directly exercises the backward scan's semicolon guard: a
        # column-0 `identifier(...)` line ending in `;` (a call/macro
        # invocation, never a signature in this codebase's convention)
        # must be skipped, not returned as the enclosing symbol, even when
        # it's the only column-0 candidate above the marker.
        lines = [
            "BB_SOME_MACRO(bb_fake, bb_fake_init);",
            "    int x = 0;",
        ]
        self.assertEqual(_enclosing_symbol(lines, 1), "?")


class TestIdentityRenameStability(unittest.TestCase):
    def test_id_unchanged_when_lines_inserted_above_marker(self):
        # The headline design claim: identity is line-shift stable — an
        # unrelated edit (blank line / comment) above a clamp must never
        # change its ratchet-diff id.
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            src = _write(root, "platform/host/bb_fake/bb_fake.c", (
                "static void set_pct(int pct)\n"
                "{\n"
                "    if (pct < 0) pct = 0;\n"
                "    if (pct > 100) pct = 100;\n"
                "}\n"
            ))
            before = scan_all(str(root))
            before_id = next(m.id for m in before if m.type == "clamp")

            src.write_text(
                "// unrelated comment\n"
                "\n"
                "static void set_pct(int pct)\n"
                "{\n"
                "    if (pct < 0) pct = 0;\n"
                "    if (pct > 100) pct = 100;\n"
                "}\n",
                encoding="utf-8",
            )
            after = scan_all(str(root))
            after_id = next(m.id for m in after if m.type == "clamp")

            self.assertEqual(before_id, after_id)


class TestExcludedDirs(unittest.TestCase):
    def test_build_and_test_dirs_skipped(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "components/bb_fake/build/generated.c", (
                "static void f(int x, int lo, int hi)\n"
                "{\n"
                "    if (x < lo) x = lo;\n"
                "    if (x > hi) x = hi;\n"
                "}\n"
            ))
            _write(root, "components/bb_fake/test/test_fake.c", (
                "static void f(int x, int lo, int hi)\n"
                "{\n"
                "    if (x < lo) x = lo;\n"
                "    if (x > hi) x = hi;\n"
                "}\n"
            ))
            found = scan_all(str(root))
            self.assertEqual(found, set())


class TestCountsByBucket(unittest.TestCase):
    def test_bucket_labels(self):
        markers = {Marker("clamp", "a.c", "bb_fake:f:x")}
        counts = counts_by_bucket(markers)
        self.assertEqual(counts, {"hand-rolled clamp": 1})


def _run_fence_cli(root: str, family=None, update_baseline: bool = False, seed=None) -> tuple:
    args = argparse.Namespace(root=root, family=family, update_baseline=update_baseline, seed=seed)
    stdout, stderr = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
        rc = fence_cmd.run(args)
    return rc, stdout.getvalue(), stderr.getvalue()


class TestFenceCliClamp(unittest.TestCase):
    """Exercises the generic `fence` CLI's shrink-only / net-new semantics
    against the real clamp family scanner, on a synthetic tree."""

    def _clamp_src(self):
        return (
            "static void set_pct(int pct)\n"
            "{\n"
            "    if (pct < 0) pct = 0;\n"
            "    if (pct > 100) pct = 100;\n"
            "}\n"
        )

    def test_seed_then_clean_run_passes(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "platform/host/bb_fake/bb_fake.c", self._clamp_src())

            rc, out, _ = _run_fence_cli(str(root), seed="clamp")
            self.assertEqual(rc, 0)
            self.assertIn("baseline seeded", out)

            rc2, out2, _ = _run_fence_cli(str(root), family=["clamp"])
            self.assertEqual(rc2, 0)
            self.assertIn("PASS", out2)

    def test_new_synthetic_clamp_fails(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "platform/host/bb_fake/bb_fake.c", self._clamp_src())
            _run_fence_cli(str(root), seed="clamp")

            _write(root, "platform/host/bb_fake/bb_fake_new.c", (
                "static void set_ms(int ms)\n"
                "{\n"
                "    if (ms < 0) ms = 0;\n"
                "    if (ms > 1000) ms = 1000;\n"
                "}\n"
            ))

            rc, out, err = _run_fence_cli(str(root), family=["clamp"])
            self.assertEqual(rc, 1)
            self.assertIn("new marker added", err)
            self.assertIn("set_ms", err)

    def test_update_baseline_does_not_bless_new_clamp(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            src = _write(root, "platform/host/bb_fake/bb_fake.c", self._clamp_src())
            _run_fence_cli(str(root), seed="clamp")

            # Simultaneously: remove the seeded clamp AND add a new one.
            src.write_text(
                "static void set_ms(int ms)\n"
                "{\n"
                "    if (ms < 0) ms = 0;\n"
                "    if (ms > 1000) ms = 1000;\n"
                "}\n",
                encoding="utf-8",
            )

            rc, out, _ = _run_fence_cli(str(root), family=["clamp"], update_baseline=True)
            self.assertEqual(rc, 0)
            self.assertIn("baseline pruned", out)
            self.assertIn("NOT added to the", out)

            baseline = fence_pkg.load_baseline(str(root), "clamp")
            baseline_ids = {m.id for m in baseline}
            self.assertNotIn("bb_fake:set_ms:ms", baseline_ids, "net-new clamp must never be blessed")

            rc2, _, err2 = _run_fence_cli(str(root), family=["clamp"])
            self.assertEqual(rc2, 1)
            self.assertIn("set_ms", err2)

    def test_migrated_site_prunes_cleanly(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            src = _write(root, "platform/host/bb_fake/bb_fake.c", self._clamp_src())
            _run_fence_cli(str(root), seed="clamp")

            # Migrate onto bb_num: the hand-rolled if-pair is gone.
            src.write_text(
                "static void set_pct(int pct)\n"
                "{\n"
                "    pct = bb_clampi(pct, 0, 100);\n"
                "}\n",
                encoding="utf-8",
            )

            rc, out, _ = _run_fence_cli(str(root), family=["clamp"])
            self.assertEqual(rc, 0, "removing a hand-rolled clamp must never fail the fence")
            self.assertIn("PASS", out)
            self.assertIn("candidate to prune from baseline", out)

            rc2, out2, _ = _run_fence_cli(str(root), family=["clamp"], update_baseline=True)
            self.assertEqual(rc2, 0)
            baseline = fence_pkg.load_baseline(str(root), "clamp")
            self.assertEqual(baseline, set())


if __name__ == "__main__":
    unittest.main()
