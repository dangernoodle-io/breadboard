"""Unit tests for commands/stage.py — direct import, no CLI deps."""
from __future__ import annotations

import os
import sys
import unittest
from pathlib import Path
from unittest.mock import patch

_BBTOOL_DIR = os.path.join(os.path.dirname(__file__), "..")
if _BBTOOL_DIR not in sys.path:
    sys.path.insert(0, _BBTOOL_DIR)

from commands.stage import stage_artifacts


def _write(path: str, content: bytes = b"fake") -> None:
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as fh:
        fh.write(content)


class TestStageArtifacts(unittest.TestCase):

    def test_bin_absent_returns_skipped(self):
        import tempfile
        with tempfile.TemporaryDirectory() as td:
            build_dir = os.path.join(td, "build")
            os.makedirs(build_dir)
            result = stage_artifacts(build_dir=build_dir, pioenv="esp32",
                                     project_dir=td)
            self.assertTrue(result["skipped"])
            self.assertIn("firmware.bin", result["reason"])

    def test_stages_with_correct_names(self):
        import tempfile
        with tempfile.TemporaryDirectory() as td:
            build_dir = os.path.join(td, "build")
            _write(os.path.join(build_dir, "firmware.bin"))
            _write(os.path.join(build_dir, "firmware.elf"))
            archive_dir = os.path.join(td, "archive")
            dist_root = os.path.join(td, "dist")

            with patch("commands.version._compute_version", return_value="0.5.0"):
                with patch("elfstore.archive", return_value="abc123") as mock_arch:
                    result = stage_artifacts(
                        build_dir=build_dir,
                        pioenv="esp32",
                        project_dir=td,
                        archive_dir=archive_dir,
                        dist_root=dist_root,
                    )

            self.assertFalse(result["skipped"])
            self.assertEqual(result["version"], "0.5.0")
            mock_arch.assert_called_once()

            dist_env = os.path.join(dist_root, "esp32")
            self.assertTrue(os.path.exists(os.path.join(dist_env, "firmware-esp32-0.5.0.bin")))
            self.assertTrue(os.path.exists(os.path.join(dist_env, "firmware-esp32-0.5.0.elf")))
            staged_names = [os.path.basename(p) for p in result["staged"]]
            self.assertIn("firmware-esp32-0.5.0.bin", staged_names)
            self.assertIn("firmware-esp32-0.5.0.elf", staged_names)

    def test_no_archive_skips_elfstore(self):
        import tempfile
        with tempfile.TemporaryDirectory() as td:
            build_dir = os.path.join(td, "build")
            _write(os.path.join(build_dir, "firmware.bin"))
            _write(os.path.join(build_dir, "firmware.elf"))
            dist_root = os.path.join(td, "dist")

            with patch("commands.version._compute_version", return_value="0.6.0"):
                with patch("elfstore.archive") as mock_arch:
                    result = stage_artifacts(
                        build_dir=build_dir,
                        pioenv="esp32",
                        project_dir=td,
                        archive=False,
                        dist_root=dist_root,
                    )

            mock_arch.assert_not_called()
            self.assertIsNone(result["archived"])
            self.assertFalse(result["skipped"])
            dist_env = os.path.join(dist_root, "esp32")
            self.assertTrue(os.path.exists(os.path.join(dist_env, "firmware-esp32-0.6.0.bin")))
            self.assertTrue(os.path.exists(os.path.join(dist_env, "firmware-esp32-0.6.0.elf")))

    def test_version_passthrough(self):
        import tempfile
        with tempfile.TemporaryDirectory() as td:
            build_dir = os.path.join(td, "build")
            _write(os.path.join(build_dir, "firmware.bin"))
            dist_root = os.path.join(td, "dist")

            with patch("commands.version._compute_version") as mock_cv:
                with patch("elfstore.archive", return_value="dead"):
                    result = stage_artifacts(
                        build_dir=build_dir,
                        pioenv="esp32s3",
                        project_dir=td,
                        archive=False,
                        dist_root=dist_root,
                        version="1.2.3",
                    )

            mock_cv.assert_not_called()
            self.assertEqual(result["version"], "1.2.3")
            dist_env = os.path.join(dist_root, "esp32s3")
            self.assertTrue(os.path.exists(os.path.join(dist_env, "firmware-esp32s3-1.2.3.bin")))


if __name__ == "__main__":
    unittest.main()
