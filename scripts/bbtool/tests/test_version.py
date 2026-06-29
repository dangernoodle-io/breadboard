"""Version command tests."""
import argparse
import os
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "commands"))

from commands.version import run, _compute_version, _write_if_changed, _bb_ref


class TestVersionOverride(unittest.TestCase):
    """BB_FW_VERSION env override takes precedence."""

    def test_env_override(self):
        with tempfile.TemporaryDirectory() as td:
            old = os.environ.get("BB_FW_VERSION")
            try:
                os.environ["BB_FW_VERSION"] = "test-1.2.3"
                v = _compute_version(td, td)
                self.assertEqual(v, "test-1.2.3")
            finally:
                if old is None:
                    os.environ.pop("BB_FW_VERSION", None)
                else:
                    os.environ["BB_FW_VERSION"] = old

    def test_emit_with_env_override(self):
        """run() with BB_FW_VERSION set prints the override string."""
        with tempfile.TemporaryDirectory() as td:
            old = os.environ.get("BB_FW_VERSION")
            try:
                os.environ["BB_FW_VERSION"] = "test-1.2.3"
                args = argparse.Namespace(
                    emit=True,
                    consumer=td,
                    bb_dir=td,
                )
                import io
                from contextlib import redirect_stdout
                buf = io.StringIO()
                with redirect_stdout(buf):
                    rc = run(args)
                self.assertEqual(rc, 0)
                self.assertEqual(buf.getvalue().strip(), "test-1.2.3")
            finally:
                if old is None:
                    os.environ.pop("BB_FW_VERSION", None)
                else:
                    os.environ["BB_FW_VERSION"] = old


class TestVersionDevFallback(unittest.TestCase):
    """Non-git directory produces dev-unknown fallback."""

    def test_dev_unknown_no_git(self):
        with tempfile.TemporaryDirectory() as td:
            old = os.environ.get("BB_FW_VERSION")
            os.environ.pop("BB_FW_VERSION", None)
            try:
                v = _compute_version(td, td)
                # Non-git dir: tm ref and bb ref both "unknown"
                self.assertTrue(v.startswith("dev-"), f"expected dev- prefix, got {v!r}")
            finally:
                if old is None:
                    os.environ.pop("BB_FW_VERSION", None)
                else:
                    os.environ["BB_FW_VERSION"] = old


class TestWriteIfChanged(unittest.TestCase):
    def test_writes_new_file(self):
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "sub" / "out.h"
            changed = _write_if_changed(str(path), "content\n")
            self.assertTrue(changed)
            self.assertEqual(path.read_text(), "content\n")

    def test_no_write_when_same(self):
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "out.h"
            path.write_text("content\n")
            changed = _write_if_changed(str(path), "content\n")
            self.assertFalse(changed)

    def test_writes_when_different(self):
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "out.h"
            path.write_text("old\n")
            changed = _write_if_changed(str(path), "new\n")
            self.assertTrue(changed)
            self.assertEqual(path.read_text(), "new\n")


class TestBbRefShatTruncation(unittest.TestCase):
    """SHA truncation for pinned fetch (.version stamp)."""

    def test_full_40_char_sha_truncated_to_7(self):
        """Full 40-char lowercase hex SHA is truncated to 7 chars."""
        with tempfile.TemporaryDirectory() as td:
            # Create .breadboard dir with .version stamp
            bb_dir = os.path.join(td, ".breadboard")
            os.makedirs(bb_dir)
            version_path = os.path.join(bb_dir, ".version")
            with open(version_path, "w") as f:
                f.write("596190b682827c8008b1df8a727f42c0b47fb4fb\n")
            result = _bb_ref(td, td)
            self.assertEqual(result, "bb-596190b")

    def test_tag_version_unchanged(self):
        """Tag/version pins (v0.70.3, 0.70.3) pass through unchanged."""
        with tempfile.TemporaryDirectory() as td:
            bb_dir = os.path.join(td, ".breadboard")
            os.makedirs(bb_dir)
            version_path = os.path.join(bb_dir, ".version")
            with open(version_path, "w") as f:
                f.write("v0.70.3\n")
            result = _bb_ref(td, td)
            self.assertEqual(result, "bb-0.70.3")

    def test_tag_version_without_v_unchanged(self):
        """Tag/version pins without leading v pass through unchanged."""
        with tempfile.TemporaryDirectory() as td:
            bb_dir = os.path.join(td, ".breadboard")
            os.makedirs(bb_dir)
            version_path = os.path.join(bb_dir, ".version")
            with open(version_path, "w") as f:
                f.write("0.70.3\n")
            result = _bb_ref(td, td)
            self.assertEqual(result, "bb-0.70.3")

    def test_short_hash_7_chars_unchanged(self):
        """Short 7-char hash is NOT shortened (it's already short)."""
        with tempfile.TemporaryDirectory() as td:
            bb_dir = os.path.join(td, ".breadboard")
            os.makedirs(bb_dir)
            version_path = os.path.join(bb_dir, ".version")
            with open(version_path, "w") as f:
                f.write("abc1234\n")
            result = _bb_ref(td, td)
            self.assertEqual(result, "bb-abc1234")

    def test_branch_name_unchanged(self):
        """Branch names pass through unchanged."""
        with tempfile.TemporaryDirectory() as td:
            bb_dir = os.path.join(td, ".breadboard")
            os.makedirs(bb_dir)
            version_path = os.path.join(bb_dir, ".version")
            with open(version_path, "w") as f:
                f.write("main\n")
            result = _bb_ref(td, td)
            self.assertEqual(result, "bb-main")

    def test_missing_version_file_returns_unknown(self):
        """Missing .version file returns unknown."""
        with tempfile.TemporaryDirectory() as td:
            bb_dir = os.path.join(td, ".breadboard")
            os.makedirs(bb_dir)
            result = _bb_ref(td, td)
            self.assertEqual(result, "bb-unknown")


if __name__ == "__main__":
    unittest.main()
