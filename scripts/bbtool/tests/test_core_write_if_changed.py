"""core.write_if_changed tests (B1-1403 review finding 3: shared extraction
of commands/version.py's and commands/codegen.py's previously-duplicate
_write_if_changed helpers) -- also covers findings 4 (unique tmp filename,
no torn writes), 5 (tmp file removed on a write failure), and 1 (permission
bits: overwriting a file must preserve its existing mode, not the
tempfile.mkstemp default of 0o600, and creating a new file must respect the
umask rather than a hardcoded 0o644)."""
import glob
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from core import write_if_changed


class TestWriteIfChanged(unittest.TestCase):
    def test_writes_new_file(self):
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "out.h"
            changed = write_if_changed(str(path), "content\n")
            self.assertTrue(changed)
            self.assertEqual(path.read_text(), "content\n")

    def test_no_write_when_same(self):
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "out.h"
            path.write_text("content\n")
            mtime_before = path.stat().st_mtime_ns
            changed = write_if_changed(str(path), "content\n")
            self.assertFalse(changed)
            self.assertEqual(path.stat().st_mtime_ns, mtime_before)

    def test_writes_when_different(self):
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "out.h"
            path.write_text("old\n")
            changed = write_if_changed(str(path), "new\n")
            self.assertTrue(changed)
            self.assertEqual(path.read_text(), "new\n")

    def test_makedirs_true_creates_parent(self):
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "sub" / "dir" / "out.h"
            changed = write_if_changed(str(path), "content\n", makedirs=True)
            self.assertTrue(changed)
            self.assertEqual(path.read_text(), "content\n")

    def test_makedirs_false_default_requires_existing_dir(self):
        """codegen.py's call sites already os.makedirs before calling this
        (pre-extraction behavior) -- default makedirs=False must not create
        a missing parent itself, or codegen.py's own os.makedirs call would
        become dead/redundant in a way that masks a future removal bug."""
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "sub" / "out.h"
            with self.assertRaises(FileNotFoundError):
                write_if_changed(str(path), "content\n")

    def test_no_leftover_tmp_file_after_success(self):
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "out.h"
            write_if_changed(str(path), "content\n")
            leftovers = [p for p in os.listdir(td) if p != "out.h"]
            self.assertEqual(leftovers, [])

    def test_tmp_file_removed_on_write_failure(self):
        """Finding 5: a write failure mid-flight must not leave a .tmp
        litter file behind in the target directory."""
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "out.h"
            with mock.patch("os.replace", side_effect=OSError("boom")):
                with self.assertRaises(OSError):
                    write_if_changed(str(path), "content\n")
            leftovers = glob.glob(os.path.join(td, "*"))
            self.assertEqual(leftovers, [])

    def test_tmp_filename_is_unique_per_call(self):
        """Finding 4: two concurrent-ish writers to the same path must never
        share a tmp filename -- each call's mkstemp-derived tmp path must
        differ, so one writer's os.replace can never promote another's
        still-partially-written tmp file."""
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "out.h"
            seen_tmp_paths = []
            real_mkstemp = tempfile.mkstemp

            def _spy_mkstemp(*args, **kwargs):
                fd, tmp_path = real_mkstemp(*args, **kwargs)
                seen_tmp_paths.append(tmp_path)
                return fd, tmp_path

            with mock.patch("tempfile.mkstemp", side_effect=_spy_mkstemp):
                write_if_changed(str(path), "one\n")
                write_if_changed(str(path), "two\n")
            self.assertEqual(len(seen_tmp_paths), 2)
            self.assertNotEqual(seen_tmp_paths[0], seen_tmp_paths[1])

    def test_overwrite_preserves_0644_mode(self):
        """Finding 1: overwriting a 0644 file must leave it 0644, not the
        tempfile.mkstemp default of 0600."""
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "out.h"
            path.write_text("old\n")
            os.chmod(path, 0o644)
            write_if_changed(str(path), "new\n")
            self.assertEqual(os.stat(path).st_mode & 0o777, 0o644)

    def test_overwrite_preserves_unusual_mode(self):
        """Finding 1: an unusual pre-existing mode (e.g. 0640) must survive
        a regeneration unchanged, not just the common 0644 case."""
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "out.h"
            path.write_text("old\n")
            os.chmod(path, 0o640)
            write_if_changed(str(path), "new\n")
            self.assertEqual(os.stat(path).st_mode & 0o777, 0o640)

    def test_new_file_mode_respects_umask_not_hardcoded_0600(self):
        """Finding 1: creating a new file must yield a sensible,
        umask-respecting mode -- never the tempfile.mkstemp default of
        0o600, which `open(path, "w")` would never have produced under a
        typical (permissive) umask."""
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "out.h"
            saved_umask = os.umask(0o022)
            os.umask(saved_umask)
            write_if_changed(str(path), "content\n")
            mode = os.stat(path).st_mode & 0o777
            self.assertEqual(mode, 0o666 & ~0o022)
            self.assertNotEqual(mode, 0o600)


if __name__ == "__main__":
    unittest.main()
