"""fence.scalar_parse family tests: bb_url_parse_bool/uint definition
scanning (fires + does not fire), family auto-discovery, and the
shrink-only baseline semantics (via the generic `fence` CLI) applied to
this concrete family."""
import os
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, os.path.dirname(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "commands"))

import fence as fence_pkg  # noqa: E402
from fence import Marker  # noqa: E402
from fence.scalar_parse import scan_all, counts_by_bucket  # noqa: E402
from fence_test_support import run_fence_cli  # noqa: E402


def _write(root: Path, rel: str, content: str) -> Path:
    path = root / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")
    return path


class TestFamilyDiscovery(unittest.TestCase):
    def test_scalar_parse_is_discovered(self):
        self.assertIn("scalar_parse", fence_pkg.FAMILIES)

    def test_baseline_path_is_per_family_convention(self):
        path = fence_pkg.baseline_path("/repo", "scalar_parse")
        self.assertEqual(path, Path("/repo/.baseline/bbtool/fence/scalar_parse.json"))


class TestScanScalarParse(unittest.TestCase):
    def test_finds_bool_and_uint_definitions(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "components/bb_fake_http/src/util.c", (
                "bool bb_url_parse_bool(const char *val, bool *out)\n"
                "{\n"
                "    return false;\n"
                "}\n"
                "\n"
                "bool bb_url_parse_uint(const char *val, unsigned long *out)\n"
                "{\n"
                "    return false;\n"
                "}\n"
            ))
            found = scan_all(str(root))
            self.assertIn(
                Marker("scalar_parse", "components/bb_fake_http/src/util.c", "bb_url_parse_bool"),
                found,
            )
            self.assertIn(
                Marker("scalar_parse", "components/bb_fake_http/src/util.c", "bb_url_parse_uint"),
                found,
            )

    def test_declaration_not_counted(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "components/bb_fake_http/include/util.h", (
                "bool bb_url_parse_bool(const char *val, bool *out);\n"
            ))
            found = scan_all(str(root))
            self.assertEqual(found, set())

    def test_multiline_wrapped_declaration_not_counted(self):
        # A prototype whose parameter list wraps across lines: the FIRST
        # line ends with `,`, not `;` — the naive "line ends with ;" check
        # would misclassify this as a definition. The balanced-paren
        # forward scan must still find the terminating `;` after the
        # closing paren, wherever it lands.
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "components/bb_fake_http/include/util.h", (
                "bool bb_url_parse_bool(const char *val,\n"
                "                        bool *out);\n"
            ))
            found = scan_all(str(root))
            self.assertEqual(found, set())

    def test_multiline_wrapped_definition_still_detected(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "components/bb_fake_http/src/util.c", (
                "bool bb_url_parse_bool(const char *val,\n"
                "                        bool *out)\n"
                "{\n"
                "    return false;\n"
                "}\n"
            ))
            found = scan_all(str(root))
            self.assertIn(
                Marker("scalar_parse", "components/bb_fake_http/src/util.c", "bb_url_parse_bool"),
                found,
            )

    def test_bb_scalar_own_definitions_excluded(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "components/bb_scalar/src/bb_scalar.c", (
                "bool bb_scalar_parse_bool(const char *val, bool *out)\n"
                "{\n"
                "    return false;\n"
                "}\n"
            ))
            _write(root, "platform/host/bb_scalar/bb_scalar.c", (
                "bool bb_url_parse_bool(const char *val, bool *out)\n"
                "{\n"
                "    return false;\n"
                "}\n"
            ))
            found = scan_all(str(root))
            self.assertEqual(found, set())

    def test_comment_reference_not_counted(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "components/bb_fake_http/src/util.c", (
                "// see bool bb_url_parse_bool(const char *val, bool *out)\n"
            ))
            found = scan_all(str(root))
            self.assertEqual(found, set())


class TestIdentityRenameStability(unittest.TestCase):
    def test_id_unchanged_when_lines_inserted_above_marker(self):
        # scalar_parse identity is purely symbol-keyed (the function name),
        # so it's trivially line-shift stable, but the design claim still
        # deserves its own regression test per family.
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            src = _write(root, "components/bb_fake_http/src/util.c", (
                "bool bb_url_parse_bool(const char *val, bool *out)\n"
                "{\n"
                "    return false;\n"
                "}\n"
            ))
            before = scan_all(str(root))
            before_id = next(m.id for m in before if m.type == "scalar_parse")

            src.write_text(
                "// unrelated comment\n"
                "\n"
                "bool bb_url_parse_bool(const char *val, bool *out)\n"
                "{\n"
                "    return false;\n"
                "}\n",
                encoding="utf-8",
            )
            after = scan_all(str(root))
            after_id = next(m.id for m in after if m.type == "scalar_parse")

            self.assertEqual(before_id, after_id)


class TestExcludedDirs(unittest.TestCase):
    def test_build_and_test_dirs_skipped(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "components/bb_fake/build/generated.c", (
                "bool bb_url_parse_bool(const char *val, bool *out)\n"
                "{\n"
                "    return false;\n"
                "}\n"
            ))
            _write(root, "components/bb_fake/test/test_fake.c", (
                "bool bb_url_parse_bool(const char *val, bool *out)\n"
                "{\n"
                "    return false;\n"
                "}\n"
            ))
            found = scan_all(str(root))
            self.assertEqual(found, set())


class TestCountsByBucket(unittest.TestCase):
    def test_bucket_labels(self):
        markers = {Marker("scalar_parse", "b.c", "bb_url_parse_bool")}
        counts = counts_by_bucket(markers)
        self.assertEqual(counts, {"hand-rolled scalar_parse": 1})


class TestFenceCliScalarParse(unittest.TestCase):
    """Exercises the generic `fence` CLI's shrink-only / net-new semantics
    against the real scalar_parse family scanner, on a synthetic tree."""

    def _parse_src(self):
        return (
            "bool bb_url_parse_bool(const char *val, bool *out)\n"
            "{\n"
            "    return false;\n"
            "}\n"
        )

    def test_seed_then_clean_run_passes(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "components/bb_fake_http/src/util.c", self._parse_src())

            rc, out, _ = run_fence_cli(str(root), seed="scalar_parse")
            self.assertEqual(rc, 0)
            self.assertIn("baseline seeded", out)

            rc2, out2, _ = run_fence_cli(str(root), family=["scalar_parse"])
            self.assertEqual(rc2, 0)
            self.assertIn("PASS", out2)

    def test_new_synthetic_definition_fails(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "components/bb_fake_http/src/util.c", self._parse_src())
            run_fence_cli(str(root), seed="scalar_parse")

            _write(root, "components/bb_fake_http2/src/util.c", (
                "bool bb_url_parse_uint(const char *val, unsigned long *out)\n"
                "{\n"
                "    return false;\n"
                "}\n"
            ))

            rc, out, err = run_fence_cli(str(root), family=["scalar_parse"])
            self.assertEqual(rc, 1)
            self.assertIn("new marker added", err)
            self.assertIn("bb_url_parse_uint", err)

    def test_update_baseline_does_not_bless_new_definition(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            src = _write(root, "components/bb_fake_http/src/util.c", self._parse_src())
            run_fence_cli(str(root), seed="scalar_parse")

            # Simultaneously: remove the seeded definition AND add a new one.
            src.write_text(
                "bool bb_url_parse_uint(const char *val, unsigned long *out)\n"
                "{\n"
                "    return false;\n"
                "}\n",
                encoding="utf-8",
            )

            rc, out, _ = run_fence_cli(str(root), family=["scalar_parse"], update_baseline=True)
            self.assertEqual(rc, 0)
            self.assertIn("baseline pruned", out)
            self.assertIn("NOT added to the", out)

            baseline = fence_pkg.load_baseline(str(root), "scalar_parse")
            baseline_ids = {m.id for m in baseline}
            self.assertNotIn("bb_url_parse_uint", baseline_ids, "net-new definition must never be blessed")

            rc2, _, err2 = run_fence_cli(str(root), family=["scalar_parse"])
            self.assertEqual(rc2, 1)
            self.assertIn("bb_url_parse_uint", err2)

    def test_migrated_site_prunes_cleanly(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            src = _write(root, "components/bb_fake_http/src/util.c", self._parse_src())
            run_fence_cli(str(root), seed="scalar_parse")

            # Migrate onto bb_scalar: the hand-rolled definition is gone.
            src.write_text(
                "#include \"bb_scalar.h\"\n"
                "// now delegates to bb_scalar_parse_bool\n",
                encoding="utf-8",
            )

            rc, out, _ = run_fence_cli(str(root), family=["scalar_parse"])
            self.assertEqual(rc, 0, "removing a hand-rolled definition must never fail the fence")
            self.assertIn("PASS", out)
            self.assertIn("candidate to prune from baseline", out)

            rc2, out2, _ = run_fence_cli(str(root), family=["scalar_parse"], update_baseline=True)
            self.assertEqual(rc2, 0)
            baseline = fence_pkg.load_baseline(str(root), "scalar_parse")
            self.assertEqual(baseline, set())


if __name__ == "__main__":
    unittest.main()
