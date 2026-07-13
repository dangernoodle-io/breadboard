"""Tests for scripts/coverage_toolchain.sh's toolchain resolver.

Exercises the two REAL invariants (not "must be gcc-16"):
  1. the resolved gcov must be genuinely GNU, not Apple/clang/LLVM (B1-867)
  2. the resolved cc/cxx/gcov must share the same major version (B1-642)

by faking a minimal, FULLY ISOLATED PATH with wrapper scripts that report
chosen `--version` output -- no real gcc/gcov install is required, and no
real gcc-N/g++-N/gcov-N on the host machine can leak in and mask a broken
fake (real Ubuntu ships genuine, matched gcc-13/g++-13/gcov-13 in /usr/bin
alongside the unversioned names -- appending /usr/bin:/bin to PATH would let
the resolver silently find and accept those instead of the fakes below).
"""
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPT = str(Path(__file__).resolve().parent.parent / "coverage_toolchain.sh")

# The only real host binaries the resolver script needs besides the
# compiler/gcov triple under test. None of these can ever resolve a
# "gcc"/"g++"/"gcov" candidate, so symlinking them in does not weaken
# isolation.
_REQUIRED_HOST_TOOLS = ("bash", "grep", "sort", "mktemp", "ln", "rm", "head", "tail", "cat")


def _write_tool(dir_path: Path, name: str, version_line: str) -> None:
    path = dir_path / name
    path.write_text(f"#!/bin/sh\necho '{version_line}'\n", encoding="utf-8")
    mode = path.stat().st_mode
    path.chmod(mode | stat.S_IEXEC | stat.S_IXGRP | stat.S_IXOTH)


def _isolated_path_dir(tmp: str) -> Path:
    """A PATH directory containing ONLY the fixed set of real coreutils the
    script needs (never a compiler name) -- fake gcc/g++/gcov tools are
    written into this same directory by the caller. Because this directory
    is the ENTIRE PATH (no /usr/bin:/bin appended), no real gcc-N/g++-N/
    gcov-N on the host can ever be discovered."""
    d = Path(tmp)
    for tool in _REQUIRED_HOST_TOOLS:
        real = shutil.which(tool)
        assert real, f"host is missing required test-harness tool: {tool}"
        (d / tool).symlink_to(real)
    return d


# Probe command run through coverage_toolchain.sh in place of the real build
# command -- an absolute path (no PATH lookup needed for /bin/sh itself), so
# it works even though PATH is fully isolated. Prints the RESOLVED toolchain
# so tests can assert on the actual paths selected, not merely on exit code.
_PROBE = [
    "/bin/sh", "-c",
    'printf \'CC=%s\\nCXX=%s\\nGCOV=%s\\n\' "$CC" "$CXX" "$COVERAGE_RESOLVED_GCOV"',
]


def _run(path_dir: Path, extra_env: dict | None = None) -> subprocess.CompletedProcess:
    env = {"PATH": str(path_dir)}
    if extra_env:
        env.update(extra_env)
    return subprocess.run(
        [SCRIPT, *_PROBE],
        env=env,
        capture_output=True,
        text=True,
        timeout=30,
    )


def _assert_resolved_under(test: unittest.TestCase, result: subprocess.CompletedProcess, path_dir: Path) -> None:
    """Assert the resolver actually picked OUR fakes (by directory), not
    merely that it exited 0 -- exit code alone can't distinguish "resolved
    my fake" from "silently resolved something else"."""
    test.assertEqual(result.returncode, 0, result.stderr)
    resolved = dict(
        line.split("=", 1) for line in result.stdout.splitlines() if "=" in line
    )
    for key in ("CC", "CXX", "GCOV"):
        test.assertIn(key, resolved, result.stdout)
        resolved_path = Path(resolved[key]).resolve()
        test.assertEqual(
            resolved_path.parent, path_dir.resolve(),
            f"{key} resolved to {resolved_path}, not our fake dir {path_dir}",
        )


@unittest.skipUnless(sys.platform != "win32", "shell script, POSIX only")
class TestResolverAcceptsMatchedGnuPairs(unittest.TestCase):
    def test_accepts_ci_shaped_unversioned_matched_gnu_pair(self):
        # Simulates ubuntu-latest CI: only unversioned gcc/g++/gcov on PATH,
        # non-16 major (13) -- the shape the old gcc-16-only resolver would
        # have rejected and bricked the required `test` check on.
        with tempfile.TemporaryDirectory() as tmp:
            d = _isolated_path_dir(tmp)
            _write_tool(d, "gcc", "gcc (Ubuntu 13.2.0-4ubuntu3) 13.2.0")
            _write_tool(d, "g++", "g++ (Ubuntu 13.2.0-4ubuntu3) 13.2.0")
            _write_tool(d, "gcov", "gcov (Ubuntu 13.2.0-4ubuntu3) 13.2.0")
            result = _run(d)
            _assert_resolved_under(self, result, d)

    def test_accepts_versioned_pair_at_any_major(self):
        # Homebrew-style versioned binaries at a major other than 16, with no
        # unversioned gcc/g++/gcov present at all -- proves the resolver
        # reaches and accepts the gcc-N branch, not merely a same-named
        # unversioned fallback.
        with tempfile.TemporaryDirectory() as tmp:
            d = _isolated_path_dir(tmp)
            _write_tool(d, "gcc-11", "gcc (Homebrew GCC 11.4.0) 11.4.0")
            _write_tool(d, "g++-11", "g++ (Homebrew GCC 11.4.0) 11.4.0")
            _write_tool(d, "gcov-11", "gcov (Homebrew GCC 11.4.0) 11.4.0")
            result = _run(d)
            _assert_resolved_under(self, result, d)
            resolved = dict(
                line.split("=", 1) for line in result.stdout.splitlines() if "=" in line
            )
            self.assertTrue(Path(resolved["CC"]).name.endswith("-11"), resolved["CC"])


class TestResolverRejectsAppleGcov(unittest.TestCase):
    def test_rejects_apple_llvm_gcov_isolated_from_version_mismatch(self):
        # Isolates the GNU-ness invariant (B1-867) from the major-version
        # invariant (B1-642): the gcov's major is made to DELIBERATELY MATCH
        # the fake gcc's (13 == 13), so the only thing left that can reject
        # this fixture is the clang|llvm|apple check in tool_is_gnu(). This
        # matters because tool_major() takes the LAST x.y.z substring on the
        # line (`grep ... | tail -1`) -- a realistic Apple `--version` string
        # carries a second, unrelated x.y.z-shaped number in its build-stamp
        # parenthetical (e.g. "(clang-1500.3.9.4)" -> "1500.3.9"), and if that
        # trailing number were picked up and happened to differ from gcc's
        # major, the resolver would reject via the version-mismatch invariant
        # instead of the GNU-ness one -- passing this test for the wrong
        # reason. Matching the majors here rules that confound out. See
        # test_rejects_apple_llvm_gcov_realistic_build_stamp below for the
        # real-world string shape.
        with tempfile.TemporaryDirectory() as tmp:
            d = _isolated_path_dir(tmp)
            _write_tool(d, "gcc", "gcc (Ubuntu 13.2.0-4ubuntu3) 13.2.0")
            _write_tool(d, "g++", "g++ (Ubuntu 13.2.0-4ubuntu3) 13.2.0")
            _write_tool(d, "gcov", "Apple LLVM version 13.0.0")
            result = _run(d)
            self.assertEqual(result.returncode, 1)
            self.assertIn("genuinely GNU", result.stderr)
            self.assertEqual(result.stdout, "")

    def test_rejects_apple_llvm_gcov_realistic_build_stamp(self):
        # Real-world Apple `clang --version` shape (build-stamp
        # parenthetical), kept alongside the isolated case above so both the
        # actual on-disk string AND the isolated invariant are covered.
        with tempfile.TemporaryDirectory() as tmp:
            d = _isolated_path_dir(tmp)
            _write_tool(d, "gcc", "gcc (Ubuntu 13.2.0-4ubuntu3) 13.2.0")
            _write_tool(d, "g++", "g++ (Ubuntu 13.2.0-4ubuntu3) 13.2.0")
            _write_tool(d, "gcov", "Apple LLVM version 15.0.0 (clang-1500.3.9.4)")
            result = _run(d)
            self.assertEqual(result.returncode, 1)
            self.assertEqual(result.stdout, "")

    def test_rejects_gcc_gcov_major_mismatch(self):
        with tempfile.TemporaryDirectory() as tmp:
            d = _isolated_path_dir(tmp)
            _write_tool(d, "gcc", "gcc (Ubuntu 13.2.0-4ubuntu3) 13.2.0")
            _write_tool(d, "g++", "g++ (Ubuntu 13.2.0-4ubuntu3) 13.2.0")
            _write_tool(d, "gcov", "gcov (Ubuntu 12.3.0-1ubuntu1) 12.3.0")
            result = _run(d)
            self.assertEqual(result.returncode, 1)
            self.assertIn("SAME MAJOR VERSION", result.stderr)
            self.assertEqual(result.stdout, "")


class TestResolverPinOptIn(unittest.TestCase):
    def test_pin_rejects_a_present_but_non_matching_major(self):
        with tempfile.TemporaryDirectory() as tmp:
            d = _isolated_path_dir(tmp)
            _write_tool(d, "gcc", "gcc (Ubuntu 13.2.0-4ubuntu3) 13.2.0")
            _write_tool(d, "g++", "g++ (Ubuntu 13.2.0-4ubuntu3) 13.2.0")
            _write_tool(d, "gcov", "gcov (Ubuntu 13.2.0-4ubuntu3) 13.2.0")
            result = _run(d, extra_env={"COVERAGE_GCC_MAJOR": "16"})
            self.assertEqual(result.returncode, 1)
            self.assertEqual(result.stdout, "")


class TestShimTempdirCleanup(unittest.TestCase):
    def test_shim_tempdir_is_removed_after_a_successful_run(self):
        with tempfile.TemporaryDirectory() as tmp, tempfile.TemporaryDirectory() as scratch:
            d = _isolated_path_dir(tmp)
            _write_tool(d, "gcc", "gcc (Ubuntu 13.2.0-4ubuntu3) 13.2.0")
            _write_tool(d, "g++", "g++ (Ubuntu 13.2.0-4ubuntu3) 13.2.0")
            _write_tool(d, "gcov", "gcov (Ubuntu 13.2.0-4ubuntu3) 13.2.0")
            result = _run(d, extra_env={"TMPDIR": scratch})
            _assert_resolved_under(self, result, d)
            leftovers = list(Path(scratch).glob("bb-coverage-toolchain.*"))
            self.assertEqual(leftovers, [], f"leaked tempdir(s): {leftovers}")


if __name__ == "__main__":
    unittest.main()
