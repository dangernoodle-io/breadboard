"""fetch command tests."""
import io
import os
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "commands"))

from commands.fetch import reconcile, DEFAULT_REPO, _is_commit_sha


def _make_components(dest):
    """Create dest/components/ to simulate a cloned breadboard."""
    Path(dest, "components").mkdir(parents=True, exist_ok=True)


def _write_stamp(dest, version):
    Path(dest, ".version").write_text(version + "\n")


class TestFetchMissingDestClones(unittest.TestCase):
    """Branch 4: DEST absent, no LOCAL → clone."""

    def test_missing_dest_clones(self):
        with tempfile.TemporaryDirectory() as td:
            dest = os.path.join(td, ".breadboard")
            with patch("subprocess.check_call") as mock_clone:
                # simulate clone creating the dest dir + stamp
                def side_effect(cmd):
                    os.makedirs(dest)
                mock_clone.side_effect = side_effect

                buf = io.StringIO()
                with redirect_stdout(buf):
                    reconcile(dest=dest, version="v1.2.3", repo=DEFAULT_REPO, local=None)
                # re-write stamp (reconcile does it after check_call)
                # actually reconcile writes it; clone side_effect only makes the dir
                # the stamp is written by reconcile itself after check_call returns

            mock_clone.assert_called_once_with(
                ["git", "clone", "--depth", "1", "--branch", "v1.2.3", DEFAULT_REPO, dest]
            )
            stamp = Path(dest, ".version").read_text()
            self.assertEqual(stamp.strip(), "v1.2.3")
            self.assertIn("fetched", buf.getvalue())


class TestFetchUpToDate(unittest.TestCase):
    """Branch 3: DEST/components exists, stamp matches → noop."""

    def test_up_to_date_noop(self):
        with tempfile.TemporaryDirectory() as td:
            dest = os.path.join(td, ".breadboard")
            os.makedirs(dest)
            _make_components(dest)
            _write_stamp(dest, "v1.2.3")

            with patch("subprocess.check_call") as mock_clone:
                buf = io.StringIO()
                with redirect_stdout(buf):
                    reconcile(dest=dest, version="v1.2.3", repo=DEFAULT_REPO, local=None)

            mock_clone.assert_not_called()
            self.assertIn("up to date", buf.getvalue())


class TestFetchStaleStampRefetches(unittest.TestCase):
    """Branch 4: DEST/components exists but stamp is old → rm + clone."""

    def test_stale_stamp_refetches(self):
        with tempfile.TemporaryDirectory() as td:
            dest = os.path.join(td, ".breadboard")
            os.makedirs(dest)
            _make_components(dest)
            _write_stamp(dest, "v1.0.0")

            def clone_side_effect(cmd):
                os.makedirs(dest)

            with patch("subprocess.check_call") as mock_clone:
                mock_clone.side_effect = clone_side_effect
                buf = io.StringIO()
                with redirect_stdout(buf):
                    reconcile(dest=dest, version="v1.2.3", repo=DEFAULT_REPO, local=None)

            mock_clone.assert_called_once()
            output = buf.getvalue()
            self.assertIn("refetching", output)
            self.assertIn("fetched", output)


class TestFetchLocalCreatesSymlink(unittest.TestCase):
    """Branch 1: LOCAL set, DEST absent → symlink created."""

    def test_local_creates_symlink(self):
        with tempfile.TemporaryDirectory() as td:
            local_src = os.path.join(td, "local_bb")
            os.makedirs(local_src)
            dest = os.path.join(td, ".breadboard")

            buf = io.StringIO()
            with redirect_stdout(buf):
                reconcile(dest=dest, version="v1.2.3", repo=DEFAULT_REPO, local=local_src)

            self.assertTrue(os.path.islink(dest))
            self.assertEqual(os.readlink(dest), os.path.abspath(local_src))
            self.assertIn("linked", buf.getvalue())


class TestFetchLocalAlreadyLinkedIdempotent(unittest.TestCase):
    """Branch 1: LOCAL set, DEST already correct symlink → no relink."""

    def test_local_already_linked_idempotent(self):
        with tempfile.TemporaryDirectory() as td:
            local_src = os.path.join(td, "local_bb")
            os.makedirs(local_src)
            dest = os.path.join(td, ".breadboard")
            os.symlink(os.path.abspath(local_src), dest)

            buf = io.StringIO()
            with redirect_stdout(buf):
                reconcile(dest=dest, version="v1.2.3", repo=DEFAULT_REPO, local=local_src)

            # Still the same symlink
            self.assertTrue(os.path.islink(dest))
            self.assertEqual(os.readlink(dest), os.path.abspath(local_src))
            self.assertIn("already linked", buf.getvalue())


class TestFetchSymlinkNoLocalLeftAsIs(unittest.TestCase):
    """Branch 2: DEST is a symlink, no LOCAL → leave as-is."""

    def test_symlink_no_local_left_as_is(self):
        with tempfile.TemporaryDirectory() as td:
            real_dir = os.path.join(td, "real_bb")
            os.makedirs(real_dir)
            dest = os.path.join(td, ".breadboard")
            os.symlink(real_dir, dest)

            with patch("subprocess.check_call") as mock_clone:
                buf = io.StringIO()
                with redirect_stdout(buf):
                    reconcile(dest=dest, version="v1.2.3", repo=DEFAULT_REPO, local=None)

            mock_clone.assert_not_called()
            self.assertIn("left as-is", buf.getvalue())


class TestIsCommitSha(unittest.TestCase):
    """_is_commit_sha: accept only full 40-hex SHAs."""

    def test_valid_sha(self):
        self.assertTrue(_is_commit_sha("a" * 40))

    def test_valid_sha_mixed(self):
        self.assertTrue(_is_commit_sha("0123456789abcdef" * 2 + "01234567"))

    def test_tag_returns_false(self):
        self.assertFalse(_is_commit_sha("v0.71.0"))

    def test_branch_returns_false(self):
        self.assertFalse(_is_commit_sha("main"))

    def test_short_sha_returns_false(self):
        self.assertFalse(_is_commit_sha("abc1234"))

    def test_empty_string_returns_false(self):
        self.assertFalse(_is_commit_sha(""))

    def test_uppercase_hex_returns_false(self):
        # only lowercase hex is valid per git convention
        self.assertFalse(_is_commit_sha("A" * 40))

    def test_39_chars_returns_false(self):
        self.assertFalse(_is_commit_sha("a" * 39))

    def test_41_chars_returns_false(self):
        self.assertFalse(_is_commit_sha("a" * 41))


_FULL_SHA = "a" * 40


class TestFetchShaDestMissingUsesInitFetch(unittest.TestCase):
    """Branch 4: dest absent, SHA version → init/remote-add/fetch/checkout sequence."""

    def test_sha_missing_dest_uses_init_sequence(self):
        with tempfile.TemporaryDirectory() as td:
            dest = os.path.join(td, ".breadboard")
            calls = []

            def fake_check_call(cmd):
                calls.append(cmd)
                if cmd[:3] == ["git", "-C", dest] and cmd[3] == "init":
                    os.makedirs(dest, exist_ok=True)

            with patch("subprocess.check_call", side_effect=fake_check_call):
                buf = io.StringIO()
                with redirect_stdout(buf):
                    reconcile(dest=dest, version=_FULL_SHA, repo=DEFAULT_REPO, local=None)

            # Must NOT use git clone --branch
            for cmd in calls:
                if "clone" in cmd:
                    self.fail(f"unexpected git clone call: {cmd}")

            # Assert the four expected subcommands in order
            self.assertEqual(calls[0], ["git", "-C", dest, "init", "-q"])
            self.assertEqual(calls[1], ["git", "-C", dest, "remote", "add", "origin", DEFAULT_REPO])
            self.assertEqual(calls[2], ["git", "-C", dest, "fetch", "--depth", "1", "origin", _FULL_SHA])
            self.assertEqual(calls[3], ["git", "-C", dest, "checkout", "-q", "FETCH_HEAD"])

            stamp = Path(dest, ".version").read_text()
            self.assertEqual(stamp.strip(), _FULL_SHA)
            self.assertIn("commit pin", buf.getvalue())


class TestFetchTagDestMissingStillUsesClone(unittest.TestCase):
    """Regression guard: tag version → git clone --depth 1 --branch, not init path."""

    def test_tag_uses_clone(self):
        with tempfile.TemporaryDirectory() as td:
            dest = os.path.join(td, ".breadboard")

            def clone_side_effect(cmd):
                os.makedirs(dest)

            with patch("subprocess.check_call") as mock_clone:
                mock_clone.side_effect = clone_side_effect
                buf = io.StringIO()
                with redirect_stdout(buf):
                    reconcile(dest=dest, version="v0.71.0", repo=DEFAULT_REPO, local=None)

            mock_clone.assert_called_once_with(
                ["git", "clone", "--depth", "1", "--branch", "v0.71.0", DEFAULT_REPO, dest]
            )
            self.assertNotIn("commit pin", buf.getvalue())


class TestFetchShaStaleDest(unittest.TestCase):
    """Stale dest + SHA version → rmtree then fetch-by-sha sequence."""

    def test_stale_dest_sha_refetches(self):
        with tempfile.TemporaryDirectory() as td:
            dest = os.path.join(td, ".breadboard")
            os.makedirs(dest)
            _make_components(dest)
            _write_stamp(dest, "a" * 40)  # old sha

            new_sha = "b" * 40
            calls = []

            def fake_check_call(cmd):
                calls.append(cmd)
                if cmd[:3] == ["git", "-C", dest] and cmd[3] == "init":
                    os.makedirs(dest, exist_ok=True)

            with patch("subprocess.check_call", side_effect=fake_check_call):
                buf = io.StringIO()
                with redirect_stdout(buf):
                    reconcile(dest=dest, version=new_sha, repo=DEFAULT_REPO, local=None)

            output = buf.getvalue()
            self.assertIn("refetching", output)
            self.assertIn("commit pin", output)
            self.assertEqual(calls[0], ["git", "-C", dest, "init", "-q"])
            stamp = Path(dest, ".version").read_text()
            self.assertEqual(stamp.strip(), new_sha)


if __name__ == "__main__":
    unittest.main()
