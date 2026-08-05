"""warning_fence_host / warning_fence_firmware family tests (B1-1420 PR2):
GCC diagnostic parsing (function-scope identity, file-scope reset, dedup),
the fail-closed live-report contract (missing/empty/unreadable/malformed
header/non-zero build_exit -> hard-fails when BB_WARNING_FENCE_STRICT=1,
SKIPs as zero otherwise), and shrink-only baseline semantics via the
generic `fence` CLI."""
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, os.path.dirname(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "commands"))

import fence as fence_pkg  # noqa: E402
from fence import Marker  # noqa: E402
from fence._gcc_warnings import (  # noqa: E402
    WarningReportInvalidError,
    parse_gcc_warnings,
    report_path,
    scan_report,
)
from fence.warning_fence_host import scan_all as scan_all_host  # noqa: E402
from fence.warning_fence_firmware import scan_all as scan_all_firmware  # noqa: E402
from fence_test_support import run_fence_cli  # noqa: E402

_VALID_HEADER = "# bb-warn-scan v1 target=host build_exit=0\n"


def _write_report(root: Path, target: str, body: str, *, build_exit: int = 0) -> Path:
    path = report_path(root, target)
    path.parent.mkdir(parents=True, exist_ok=True)
    header = f"# bb-warn-scan v1 target={target} build_exit={build_exit}\n"
    path.write_text(header + body, encoding="utf-8")
    return path


def _not_strict():
    return mock.patch.dict(os.environ, {"BB_WARNING_FENCE_STRICT": ""})


def _strict():
    return mock.patch.dict(os.environ, {"BB_WARNING_FENCE_STRICT": "1"})


class TestFamilyDiscovery(unittest.TestCase):
    def test_both_families_are_discovered(self):
        self.assertIn("warning_fence_host", fence_pkg.FAMILIES)
        self.assertIn("warning_fence_firmware", fence_pkg.FAMILIES)

    def test_shared_helper_module_is_not_itself_a_family(self):
        self.assertNotIn("_gcc_warnings", fence_pkg.FAMILIES)

    def test_baseline_paths_are_per_family_convention(self):
        self.assertEqual(
            fence_pkg.baseline_path("/repo", "warning_fence_host"),
            Path("/repo/.baseline/bbtool/fence/warning_fence_host.json"),
        )
        self.assertEqual(
            fence_pkg.baseline_path("/repo", "warning_fence_firmware"),
            Path("/repo/.baseline/bbtool/fence/warning_fence_firmware.json"),
        )


class TestParseGccWarnings(unittest.TestCase):
    def test_function_scoped_warning_keys_on_enclosing_function(self):
        text = (
            "components/bb_fake/bb_fake.c: In function 'bb_fake_init':\n"
            "components/bb_fake/bb_fake.c:12:5: warning: unused variable 'n' [-Wunused-variable]\n"
        )
        found = parse_gcc_warnings(text)
        self.assertEqual(
            found,
            {Marker("unused-variable", "components/bb_fake/bb_fake.c", "bb_fake_init: unused variable 'n'")},
        )

    def test_file_scope_warning_has_no_function_prefix(self):
        text = (
            "components/bb_fake/bb_fake.c: At top level:\n"
            "components/bb_fake/bb_fake.c:3:13: warning: 'TAG' defined but not used [-Wunused-variable]\n"
        )
        found = parse_gcc_warnings(text)
        self.assertEqual(
            found,
            {Marker("unused-variable", "components/bb_fake/bb_fake.c", "'TAG' defined but not used")},
        )

    def test_missing_context_header_resets_scope_on_file_transition(self):
        text = (
            "a.c: In function 'f':\n"
            "a.c:1:1: warning: unused variable 'x' [-Wunused-variable]\n"
            "b.c:2:1: warning: 'TAG' defined but not used [-Wunused-variable]\n"
        )
        found = parse_gcc_warnings(text)
        self.assertIn(Marker("unused-variable", "b.c", "'TAG' defined but not used"), found)
        self.assertNotIn(Marker("unused-variable", "b.c", "f: 'TAG' defined but not used"), found)

    def test_two_identical_messages_in_different_functions_are_distinct(self):
        text = (
            "x.c: In function 'f1':\n"
            "x.c:10:5: warning: missing braces around initializer [-Wmissing-braces]\n"
            "x.c: In function 'f2':\n"
            "x.c:20:5: warning: missing braces around initializer [-Wmissing-braces]\n"
        )
        found = parse_gcc_warnings(text)
        self.assertEqual(len(found), 2)

    def test_line_number_is_not_part_of_identity(self):
        before = "x.c: In function 'f':\nx.c:10:5: warning: unused variable 'n' [-Wunused-variable]\n"
        after = "x.c: In function 'f':\nx.c:99:5: warning: unused variable 'n' [-Wunused-variable]\n"
        self.assertEqual(parse_gcc_warnings(before), parse_gcc_warnings(after))

    def test_repeated_header_only_warning_dedups_to_one_marker(self):
        text = (
            "components/bb_fake/include/bb_fake.h:5:1: warning: '/*' within comment [-Wcomment]\n"
            "components/bb_fake/include/bb_fake.h:5:1: warning: '/*' within comment [-Wcomment]\n"
        )
        found = parse_gcc_warnings(text)
        self.assertEqual(len(found), 1)

    def test_unflagged_diagnostic_falls_back_gracefully(self):
        text = "x.c:1:1: warning: something odd with no bracket tag\n"
        found = parse_gcc_warnings(text)
        self.assertEqual(found, {Marker("unflagged", "x.c", "something odd with no bracket tag")})

    def test_non_diagnostic_lines_are_ignored(self):
        text = (
            "Building...\n"
            "In file included from x.c:1:\n"
            "----------------- native:test_host [PASSED] -----------------\n"
        )
        self.assertEqual(parse_gcc_warnings(text), set())

    def test_all_quote_glyph_forms_key_identically(self):
        # B1-1420 PR2 follow-up: gcc's diagnostic quote glyph around a
        # symbol/token varies by gcc version, build, and message-catalog
        # state -- curly single (U+2018/U+2019), curly double (U+201C/
        # U+201D), straight double ("), and straight ASCII single (') all
        # appear across real gcc builds for what is otherwise the exact
        # same warning (PR #1231's CI run hit both curly-single, via
        # locale, and straight-double, via a hardcoded `-Wcomment` format
        # string that never used %qs at all). All four must key to the
        # SAME marker. Mutation that makes this fail: delete the
        # `_normalize_quotes` call on `line` in `parse_gcc_warnings`.
        ascii_single_text = (
            "a.c: In function 'f':\n"
            "a.c:1:1: warning: 'TAG' defined but not used [-Wunused-variable]\n"
        )
        curly_single_text = (
            "a.c: In function ‘f’:\n"
            "a.c:1:1: warning: ‘TAG’ defined but not used [-Wunused-variable]\n"
        )
        curly_double_text = (
            "a.c: In function “f”:\n"
            "a.c:1:1: warning: “TAG” defined but not used [-Wunused-variable]\n"
        )
        ascii_double_text = (
            'a.c: In function "f":\n'
            'a.c:1:1: warning: "TAG" defined but not used [-Wunused-variable]\n'
        )
        expected = {Marker("unused-variable", "a.c", "f: 'TAG' defined but not used")}
        for text in (ascii_single_text, curly_single_text, curly_double_text, ascii_double_text):
            self.assertEqual(parse_gcc_warnings(text), expected)

    def test_curly_quotes_normalized_without_symbol_name_stripped(self):
        # Layer-2 guard: normalization must map the glyph only, never
        # strip the quoted symbol name itself out of the key.
        text = "a.c:1:1: warning: unused variable ‘n’ [-Wunused-variable]\n"
        found = parse_gcc_warnings(text)
        self.assertEqual(found, {Marker("unused-variable", "a.c", "unused variable 'n'")})


class TestFailClosedReportContract(unittest.TestCase):
    """The whole point of this rewrite: a missing/empty/unreadable/
    malformed/build-failed report is NEVER silently treated as zero
    warnings when BB_WARNING_FENCE_STRICT=1 -- otherwise it's a loud
    SKIP."""

    def test_missing_report_skips_as_empty_when_not_strict(self):
        with tempfile.TemporaryDirectory() as td, _not_strict():
            self.assertEqual(scan_report(Path(td), "host"), set())

    def test_missing_report_hard_fails_when_strict(self):
        with tempfile.TemporaryDirectory() as td, _strict():
            with self.assertRaises(WarningReportInvalidError):
                scan_report(Path(td), "host")

    def test_empty_report_hard_fails_when_strict(self):
        with tempfile.TemporaryDirectory() as td, _strict():
            path = report_path(Path(td), "host")
            path.parent.mkdir(parents=True)
            path.write_text("", encoding="utf-8")
            with self.assertRaises(WarningReportInvalidError):
                scan_report(Path(td), "host")

    def test_malformed_header_hard_fails_when_strict(self):
        with tempfile.TemporaryDirectory() as td, _strict():
            path = report_path(Path(td), "host")
            path.parent.mkdir(parents=True)
            path.write_text("not a valid header at all\n", encoding="utf-8")
            with self.assertRaises(WarningReportInvalidError):
                scan_report(Path(td), "host")

    def test_nonzero_build_exit_hard_fails_when_strict_even_with_no_diagnostics(self):
        # The exact bug this rewrite closes: a build that broke before it
        # could even emit diagnostics must NOT look like "zero warnings".
        with tempfile.TemporaryDirectory() as td, _strict():
            _write_report(Path(td), "host", "", build_exit=1)
            with self.assertRaises(WarningReportInvalidError):
                scan_report(Path(td), "host")

    def test_zero_build_exit_with_no_diagnostics_is_a_real_pass_when_strict(self):
        # The legitimate all-clear state (after the backlog is fully
        # cleared) must NOT be mistaken for the failure case above.
        with tempfile.TemporaryDirectory() as td, _strict():
            _write_report(Path(td), "host", "", build_exit=0)
            self.assertEqual(scan_report(Path(td), "host"), set())

    def test_valid_report_parses_normally_when_strict(self):
        with tempfile.TemporaryDirectory() as td, _strict():
            _write_report(
                Path(td), "host",
                "a.c:1:1: warning: unused variable 'h' [-Wunused-variable]\n",
            )
            found = scan_report(Path(td), "host")
            self.assertEqual(found, {Marker("unused-variable", "a.c", "unused variable 'h'")})


class TestScanAllPerFamily(unittest.TestCase):
    def test_host_family_reads_only_the_host_report(self):
        with tempfile.TemporaryDirectory() as td, _not_strict():
            root = Path(td)
            _write_report(root, "host", "a.c:1:1: warning: unused variable 'h' [-Wunused-variable]\n")
            _write_report(root, "firmware", "b.c:1:1: warning: unused variable 'f' [-Wunused-variable]\n")
            found = scan_all_host(str(root))
            self.assertEqual(found, {Marker("unused-variable", "a.c", "unused variable 'h'")})

    def test_firmware_family_reads_only_the_firmware_report(self):
        with tempfile.TemporaryDirectory() as td, _not_strict():
            root = Path(td)
            _write_report(root, "host", "a.c:1:1: warning: unused variable 'h' [-Wunused-variable]\n")
            _write_report(root, "firmware", "b.c:1:1: warning: unused variable 'f' [-Wunused-variable]\n")
            found = scan_all_firmware(str(root))
            self.assertEqual(found, {Marker("unused-variable", "b.c", "unused variable 'f'")})


class TestFenceCliWarningFenceHost(unittest.TestCase):
    """Exercises the generic `fence` CLI's shrink-only / net-new semantics
    against the real warning_fence_host scanner, on a synthetic report."""

    def test_seed_then_clean_run_passes(self):
        with tempfile.TemporaryDirectory() as td, _not_strict():
            root = Path(td)
            _write_report(root, "host", "a.c:1:1: warning: unused variable 'h' [-Wunused-variable]\n")

            rc, out, _ = run_fence_cli(str(root), seed="warning_fence_host")
            self.assertEqual(rc, 0)
            self.assertIn("baseline seeded", out)

            rc2, out2, _ = run_fence_cli(str(root), family=["warning_fence_host"])
            self.assertEqual(rc2, 0)
            self.assertIn("PASS", out2)

    def test_new_warning_fails(self):
        with tempfile.TemporaryDirectory() as td, _not_strict():
            root = Path(td)
            _write_report(root, "host", "a.c:1:1: warning: unused variable 'h' [-Wunused-variable]\n")
            run_fence_cli(str(root), seed="warning_fence_host")

            _write_report(
                root, "host",
                "a.c:1:1: warning: unused variable 'h' [-Wunused-variable]\n"
                "a.c:2:1: warning: 'g' defined but not used [-Wunused-function]\n",
            )

            rc, out, err = run_fence_cli(str(root), family=["warning_fence_host"])
            self.assertEqual(rc, 1)
            self.assertIn("new marker added", err)
            self.assertIn("a.c", err)

    def test_update_baseline_does_not_bless_new_warning(self):
        with tempfile.TemporaryDirectory() as td, _not_strict():
            root = Path(td)
            _write_report(root, "host", "a.c:1:1: warning: unused variable 'h' [-Wunused-variable]\n")
            run_fence_cli(str(root), seed="warning_fence_host")

            _write_report(root, "host", "a.c:2:1: warning: 'g' defined but not used [-Wunused-function]\n")

            rc, out, _ = run_fence_cli(str(root), family=["warning_fence_host"], update_baseline=True)
            self.assertEqual(rc, 0)
            self.assertIn("baseline pruned", out)
            self.assertIn("NOT added to the", out)

            rc2, _, err2 = run_fence_cli(str(root), family=["warning_fence_host"])
            self.assertEqual(rc2, 1)
            self.assertIn("a.c", err2)

    def test_removed_warning_prunes_cleanly(self):
        with tempfile.TemporaryDirectory() as td, _not_strict():
            root = Path(td)
            _write_report(root, "host", "a.c:1:1: warning: unused variable 'h' [-Wunused-variable]\n")
            run_fence_cli(str(root), seed="warning_fence_host")

            _write_report(root, "host", "")

            rc, out, _ = run_fence_cli(str(root), family=["warning_fence_host"])
            self.assertEqual(rc, 0, "clearing a warning must never fail the fence")
            self.assertIn("PASS", out)
            self.assertIn("candidate to prune from baseline", out)

            rc2, out2, _ = run_fence_cli(str(root), family=["warning_fence_host"], update_baseline=True)
            self.assertEqual(rc2, 0)
            baseline = fence_pkg.load_baseline(str(root), "warning_fence_host")
            self.assertEqual(baseline, set())

    def test_update_baseline_never_prunes_a_skip_mode_empty_scan(self):
        # The exact bug from review: `--update-baseline` with no `--family`
        # filter, run on a dev machine that hasn't run warn_scan.sh (or in
        # the toolchain-less `check` job), must NEVER read a SKIP's empty
        # `current` as genuine shrinkage. Mutation that makes this test
        # fail: delete the `_scan_is_trustworthy` guard in
        # `commands/fence_cmd.py::_update_baseline` (restoring the
        # unconditional prune path) -- the baseline then goes 2 -> 0
        # instead of staying at 2.
        with tempfile.TemporaryDirectory() as td, _not_strict():
            root = Path(td)
            _write_report(
                root, "host",
                "a.c:1:1: warning: unused variable 'h' [-Wunused-variable]\n"
                "b.c:2:2: warning: unused variable 'g' [-Wunused-variable]\n",
            )
            run_fence_cli(str(root), seed="warning_fence_host")
            baseline_before = fence_pkg.load_baseline(str(root), "warning_fence_host")
            self.assertEqual(len(baseline_before), 2)

            # No report at all now -- SKIP mode, not "zero warnings found".
            report_path(root, "host").unlink()

            rc, out, _ = run_fence_cli(str(root), family=["warning_fence_host"], update_baseline=True)
            self.assertEqual(rc, 0)
            self.assertIn("skipping --update-baseline", out)
            self.assertNotIn("baseline pruned", out)

            baseline_after = fence_pkg.load_baseline(str(root), "warning_fence_host")
            self.assertEqual(
                baseline_after, baseline_before,
                "a SKIP-mode scan must never prune the baseline",
            )

            # And a plain (non---update-baseline) check still passes clean,
            # with the noise suppressed rather than 2 misleading
            # "candidate to prune" lines (review nit #3).
            rc2, out2, _ = run_fence_cli(str(root), family=["warning_fence_host"])
            self.assertEqual(rc2, 0)
            self.assertIn("PASS", out2)
            self.assertNotIn("candidate to prune from baseline", out2)
            self.assertIn("SKIP", out2)

    def test_baseline_entry_with_stale_glyph_matches_canonical_scan(self):
        # PR #1231's actual CI failure: `warning_fence_firmware.json` was
        # seeded (before quote-glyph normalization existed) with a
        # double-quoted id ("/*" within comment); `parse_gcc_warnings` now
        # always produces the canonical single-quoted form. Without
        # normalizing the BASELINE side too, the two can never match no
        # matter how the scan side is normalized. Mutation that makes this
        # fail: drop the `normalize_id=getattr(module, "normalize_id",
        # None)` argument from either `load_baseline(...)` call in
        # `commands/fence_cmd.py` (`_check`/`_update_baseline`), reverting
        # to normalizing only the live scan.
        with tempfile.TemporaryDirectory() as td, _not_strict():
            root = Path(td)
            fence_pkg.baseline_path(str(root), "warning_fence_host").parent.mkdir(
                parents=True, exist_ok=True
            )
            fence_pkg.baseline_path(str(root), "warning_fence_host").write_text(
                '{"entries": [{"type": "comment", "path": "a.h", "id": "\\"/*\\" within comment"}]}\n',
                encoding="utf-8",
            )
            baseline_on_disk_before = fence_pkg.baseline_path(str(root), "warning_fence_host").read_text(
                encoding="utf-8"
            )

            # The live scan produces the canonical (single-quote) form --
            # same underlying warning, different glyph than the baseline
            # entry above.
            _write_report(root, "host", "a.h:5:1: warning: \"/*\" within comment [-Wcomment]\n")

            rc, out, err = run_fence_cli(str(root), family=["warning_fence_host"])
            self.assertEqual(rc, 0, f"expected PASS, got FAIL: {err}")
            self.assertIn("0 new", out)

            # The committed baseline file itself is untouched by a plain
            # check -- normalization happens only in the in-memory Marker
            # `load_baseline` returns, never by rewriting the file on read.
            self.assertEqual(
                fence_pkg.baseline_path(str(root), "warning_fence_host").read_text(encoding="utf-8"),
                baseline_on_disk_before,
            )


if __name__ == "__main__":
    unittest.main()
