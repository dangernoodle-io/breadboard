"""Integration test: real repo passes clean at --profile library."""
import argparse
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "commands"))

from commands.lint import run as lint_run

REPO_ROOT = os.path.abspath(
    os.path.join(os.path.dirname(__file__), "..", "..", "..")
)


class TestRealRepoClean(unittest.TestCase):
    def test_real_repo_passes_at_library_profile(self):
        """make check equivalent: real repo must exit 0 at library profile."""
        args = argparse.Namespace(
            root=REPO_ROOT,
            profile="library",
            rules=None,
            list=False,
            _config_dict={},
            _root_abs=REPO_ROOT,
        )
        rc = lint_run(args)
        self.assertEqual(rc, 0, f"real repo must pass lint at library profile (root={REPO_ROOT})")


if __name__ == "__main__":
    unittest.main()
