"""di-fence command tests: back-compat alias for `fence --family di_legacy`.

Scanner/baseline/diff logic itself is tested in test_fence.py (engine) and
test_fence_di_legacy.py (family scanners); this file only proves the alias
wires through correctly and produces the same pass/fail behavior.
"""
import argparse
import contextlib
import io
import os
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, os.path.dirname(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "commands"))

import fence as fence_pkg  # noqa: E402
from commands import di_fence  # noqa: E402
from discovery import build_index  # noqa: E402
from fence_test_support import run_fence_cli as _run_fence  # noqa: E402


def _write(root: Path, rel: str, content: str) -> Path:
    path = root / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")
    return path


def _run_di_fence(root: str, update_baseline: bool = False) -> tuple:
    # See `tests/fence_test_support.py`: these tests mutate the SAME tmp
    # root across multiple calls within one process, so the memoized
    # `discovery.build_index()` cache must be cleared per-invocation here
    # too (di_fence.run() delegates straight to fence_cmd.run(), which no
    # longer clears it itself — B1-1128).
    build_index.cache_clear()
    args = argparse.Namespace(root=root, update_baseline=update_baseline)
    stdout, stderr = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
        rc = di_fence.run(args)
    return rc, stdout.getvalue(), stderr.getvalue()


class TestDiFenceAliasEquivalence(unittest.TestCase):
    def test_alias_matches_fence_family_di_legacy_pass(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "components/bb_fake/src/bb_fake.c", "BB_INIT_REGISTER(bb_fake, bb_fake_init);\n")
            _run_fence(str(root), seed="di_legacy")

            rc_alias, out_alias, _ = _run_di_fence(str(root))
            rc_fence, out_fence, _ = _run_fence(str(root), family=["di_legacy"])

            self.assertEqual(rc_alias, 0)
            self.assertEqual(rc_alias, rc_fence)
            self.assertIn("PASS", out_alias)
            self.assertIn("PASS", out_fence)

    def test_alias_matches_fence_family_di_legacy_fail(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            src = _write(root, "components/bb_fake/src/bb_fake.c", "BB_INIT_REGISTER(bb_fake, bb_fake_init);\n")
            _run_fence(str(root), seed="di_legacy")

            src.write_text((
                "BB_INIT_REGISTER(bb_fake, bb_fake_init);\n"
                "BB_INIT_REGISTER(bb_fake_new, bb_fake_new_init);\n"
            ), encoding="utf-8")

            rc_alias, _, err_alias = _run_di_fence(str(root))
            rc_fence, _, err_fence = _run_fence(str(root), family=["di_legacy"])

            self.assertEqual(rc_alias, 1)
            self.assertEqual(rc_alias, rc_fence)
            self.assertIn("bb_fake_new", err_alias)
            self.assertIn("bb_fake_new", err_fence)

    def test_alias_update_baseline_is_shrink_only(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            src = _write(root, "components/bb_fake/src/bb_fake.c", (
                "BB_INIT_REGISTER(bb_fake, bb_fake_init);\n"
                "BB_INIT_REGISTER(bb_fake_gone, bb_fake_gone_init);\n"
            ))
            _run_fence(str(root), seed="di_legacy")

            src.write_text((
                "BB_INIT_REGISTER(bb_fake, bb_fake_init);\n"
                "BB_INIT_REGISTER(bb_fake_new_dup, bb_fake_new_dup_init);\n"
            ), encoding="utf-8")

            rc, out, _ = _run_di_fence(str(root), update_baseline=True)
            self.assertEqual(rc, 0)

            baseline_ids = {m.id for m in fence_pkg.load_baseline(str(root), "di_legacy")}
            self.assertNotIn("bb_fake_gone", baseline_ids)
            self.assertNotIn("bb_fake_new_dup", baseline_ids, "net-new marker must never be blessed")

    def test_uses_di_legacy_baseline_path(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "components/bb_fake/src/bb_fake.c", "BB_INIT_REGISTER(bb_fake, bb_fake_init);\n")
            _run_fence(str(root), seed="di_legacy")
            self.assertTrue((root / ".baseline" / "bbtool" / "fence" / "di_legacy.json").is_file())


class TestDiFenceAddArguments(unittest.TestCase):
    def test_parses_update_baseline_flag(self):
        parser = argparse.ArgumentParser()
        di_fence.add_arguments(parser)
        ns = parser.parse_args(["--update-baseline"])
        self.assertTrue(ns.update_baseline)
        ns2 = parser.parse_args([])
        self.assertFalse(ns2.update_baseline)


class TestRealBaselinePasses(unittest.TestCase):
    """Equivalence gate: on the actual breadboard worktree, `di-fence` (the
    alias) must PASS cleanly, since the migrated di_legacy baseline is the
    same content as the pre-migration baseline."""

    def test_di_fence_passes_on_repo_root(self):
        repo_root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
        if not (Path(repo_root) / "components").is_dir():
            self.skipTest("not running inside the breadboard repo tree")
        rc, out, err = _run_di_fence(repo_root)
        self.assertEqual(rc, 0, f"stdout={out!r} stderr={err!r}")
        self.assertIn("PASS", out)


if __name__ == "__main__":
    unittest.main()
