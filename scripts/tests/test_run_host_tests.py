"""Regression fixture for scripts/run_host_tests.py (B1-1137).

`pio test`'s own asyncio test-output reader can misreport a genuine test
failure as a pass (see scripts/run_host_tests.py's module docstring for the
two confirmed upstream defects: Bug A -- exit code misread as a signal via
`signal.Signals(abs(return_code))`; Bug B -- the buffered stdout tail lost to
an asyncio process_exited()/pipe_data_received() race). Neither bug can be
reproduced through the real `pio` CLI in a unit test without vendoring
PlatformIO itself, so this fixture proves the fix at the level the task
allows: stub Unity-shaped binaries drive `evaluate_binary()` directly
(bypassing `pio test` entirely, exactly like the fixed Makefile path does),
and a literal reproduction of Bug A's buggy snippet demonstrates the OLD
verdict logic misreading the very same exit code that `evaluate_binary()`
correctly reads as a failure.
"""
import signal
import stat
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import run_host_tests  # noqa: E402


def _write_stub_binary(dir_path: Path, name: str, script: str) -> Path:
    path = dir_path / name
    path.write_text(script, encoding="utf-8")
    mode = path.stat().st_mode
    path.chmod(mode | stat.S_IEXEC | stat.S_IXGRP | stat.S_IXOTH)
    return path


class TestEvaluateBinaryGenuinePassAndFail(unittest.TestCase):
    def test_clean_pass_exit_zero_zero_failures(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary = _write_stub_binary(
                Path(tmp), "program",
                "#!/bin/sh\n"
                "echo 'test/test_host/test_main.c:1: test_ok\t[PASSED]'\n"
                "echo\n"
                "echo '-----------------------'\n"
                "echo '3 Tests 0 Failures 0 Ignored'\n"
                "echo OK\n"
                "exit 0\n",
            )
            result = run_host_tests.evaluate_binary(binary)
            self.assertTrue(result.passed, result.message)

    def test_genuine_failure_is_reported_as_fail(self):
        # Mirrors Unity's real exit-status contract: `main()` returns
        # UNITY_END() -- the FAILURE COUNT -- as the process exit code. One
        # failing test out of three: exit(1), summary reports 1 failure.
        with tempfile.TemporaryDirectory() as tmp:
            binary = _write_stub_binary(
                Path(tmp), "program",
                "#!/bin/sh\n"
                "echo 'test/test_host/test_main.c:1: test_ok\t[PASSED]'\n"
                "echo 'test/test_host/test_main.c:2: test_broken\t[FAILED]'\n"
                "echo 'test/test_host/test_main.c:3: test_ok2\t[PASSED]'\n"
                "echo\n"
                "echo '-----------------------'\n"
                "echo '3 Tests 1 Failures 0 Ignored'\n"
                "echo FAIL\n"
                "exit 1\n",
            )
            result = run_host_tests.evaluate_binary(binary)
            self.assertFalse(result.passed, result.message)
            self.assertIn("exit=1", result.message)
            self.assertIn("1 Failures", result.message)


class TestEvaluateBinaryMissingSummaryNeverPasses(unittest.TestCase):
    def test_missing_summary_line_is_fail_even_on_clean_exit(self):
        # Simulates Bug B (dropped trailing output): the process exits 0 but
        # its summary line never made it into the captured output (e.g. a
        # crash mid-teardown after the last per-case line, or truncated
        # capture upstream). A missing summary must never read as a pass.
        with tempfile.TemporaryDirectory() as tmp:
            binary = _write_stub_binary(
                Path(tmp), "program",
                "#!/bin/sh\n"
                "echo 'test/test_host/test_main.c:1: test_ok\t[PASSED]'\n"
                "exit 0\n",
            )
            result = run_host_tests.evaluate_binary(binary)
            self.assertFalse(result.passed, result.message)
            self.assertIn("no Unity summary line found", result.message)

    def test_missing_binary_is_fail(self):
        result = run_host_tests.evaluate_binary(Path("/nonexistent/program-B1-1137"))
        self.assertFalse(result.passed)
        self.assertIn("not found", result.message)


class TestEvaluateBinaryInconsistencyIsFail(unittest.TestCase):
    def test_zero_exit_with_nonzero_failures_in_summary_is_fail(self):
        # A defensive belt-and-braces case: if the exit code and the Unity
        # summary ever disagree, trust neither -- fail loudly rather than
        # silently pick one.
        with tempfile.TemporaryDirectory() as tmp:
            binary = _write_stub_binary(
                Path(tmp), "program",
                "#!/bin/sh\n"
                "echo '2 Tests 1 Failures 0 Ignored'\n"
                "exit 0\n",
            )
            result = run_host_tests.evaluate_binary(binary)
            self.assertFalse(result.passed, result.message)


class TestBugAReproduction(unittest.TestCase):
    """Literal reproduction of PlatformIO 6.1.19's
    `NativeTestOutputReader.raise_for_status()` bug: it calls
    `signal.Signals(abs(return_code))` on ANY nonzero return code, with no
    check for whether the child died by signal (negative returncode) or
    exited normally via `exit(N)` (positive) -- Unity's own exit-code
    contract. This proves the OLD verdict path actively misreads the exact
    same exit code `evaluate_binary()` correctly treats as a failure."""

    def _buggy_pio_style_status(self, return_code: int) -> str:
        # platformio/test/runners/readers/native.py, unpatched (B1-1137):
        # no sign check before feeding a positive UNITY_END() failure count
        # into signal.Signals().
        return signal.Signals(abs(return_code)).name

    def test_old_reader_mislabels_a_unity_failure_count_as_a_signal(self):
        # Unity exit(1) for "one test failed" -- the buggy snippet resolves
        # this to SIGHUP, not "1 test failed".
        self.assertEqual(self._buggy_pio_style_status(1), "SIGHUP")

    def test_new_evaluate_binary_never_reinterprets_exit_code_as_a_signal(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary = _write_stub_binary(
                Path(tmp), "program",
                "#!/bin/sh\n"
                "echo '1 Tests 1 Failures 0 Ignored'\n"
                "exit 1\n",
            )
            result = run_host_tests.evaluate_binary(binary)
            self.assertFalse(result.passed)
            # The fixed verdict path reports the Unity failure count, never
            # PlatformIO's spurious signal-name misinterpretation.
            self.assertNotIn("SIG", result.message)
            self.assertIn("1 Failures", result.message)


class TestEvaluateBinaryCapturesFullBufferedTail(unittest.TestCase):
    def test_output_flushed_only_at_exit_is_still_captured_whole(self):
        # Bug B is a race specific to PlatformIO's asyncio reader ending the
        # run on process_exited() concurrently with the final
        # pipe_data_received() delivery. subprocess.run() has no such race:
        # it reads to EOF on the child's stdout pipe, which only happens
        # after the child (and therefore libc's atexit flush) has fully
        # exited -- so a summary line written just before a bare `exit()`
        # (no explicit flush call) is still reliably captured.
        with tempfile.TemporaryDirectory() as tmp:
            binary = _write_stub_binary(
                Path(tmp), "program",
                "#!/bin/sh\n"
                "printf 'padding line %d\\n' $(seq 1 500)\n"
                "printf '1 Tests 0 Failures 0 Ignored\\n'\n"
                "exit 0\n",
            )
            result = run_host_tests.evaluate_binary(binary)
            self.assertTrue(result.passed, result.message)


class TestEvaluateBinaryZeroTestsExecutedIsFail(unittest.TestCase):
    def test_zero_tests_executed_never_passes(self):
        # Unity misconfigured, or a test file silently dropped from the
        # build: RUN_TEST is never invoked, so the binary exits 0 with a
        # summary line reporting zero failures -- but it also ran nothing.
        # This is the canonical "check silently stopped covering something"
        # shape and must never read as a pass.
        with tempfile.TemporaryDirectory() as tmp:
            binary = _write_stub_binary(
                Path(tmp), "program",
                "#!/bin/sh\n"
                "echo '0 Tests 0 Failures 0 Ignored'\n"
                "exit 0\n",
            )
            result = run_host_tests.evaluate_binary(binary)
            self.assertFalse(result.passed, result.message)


class TestEvaluateBinaryTimeout(unittest.TestCase):
    def test_hanging_binary_is_treated_as_a_failure_not_a_forever_block(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary = _write_stub_binary(
                Path(tmp), "program",
                "#!/bin/sh\nsleep 5\necho '1 Tests 0 Failures 0 Ignored'\nexit 0\n",
            )
            result = run_host_tests.evaluate_binary(binary, timeout=0.2)
            self.assertFalse(result.passed)
            self.assertIn("did not complete", result.message)


class TestEvaluateBinaryNotExecutable(unittest.TestCase):
    def test_non_executable_file_is_a_clean_fail_not_a_traceback(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "program"
            path.write_text("not actually executable", encoding="utf-8")
            path.chmod(stat.S_IRUSR | stat.S_IWUSR)  # no exec bits at all
            result = run_host_tests.evaluate_binary(path)
            self.assertFalse(result.passed)
            self.assertIn("not executable", result.message)


class TestRunEnvRequiresBothPioExitAndBinaryVerdict(unittest.TestCase):
    """The HIGH finding this class guards against: a build failure (e.g. a
    syntax error) makes `pio test` exit nonzero, but SCons stops before link,
    leaving a PREVIOUSLY built, genuinely-passing binary untouched on disk.
    evaluate_binary() in isolation cannot see this -- it faithfully reports
    PASS on the stale binary it was handed. run_env() must therefore require
    pio's own exit code to be clean too, or this is a worse false-green than
    the one B1-1137 exists to fix."""

    def test_failing_pio_build_with_a_stale_passing_binary_is_fail(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            build_dir = root / ".pio" / "build" / "native"
            build_dir.mkdir(parents=True)
            # A stale binary from a PRIOR successful build -- genuinely
            # reports a clean pass if run directly.
            _write_stub_binary(
                build_dir, "program",
                "#!/bin/sh\necho '4714 Tests 0 Failures 0 Ignored'\nexit 0\n",
            )
            # Simulates `pio test` failing at the BUILD step (e.g. a syntax
            # error) -- exits nonzero, never touches the stale binary above.
            pio_stub = _write_stub_binary(
                root, "pio-stub",
                "#!/bin/sh\necho 'simulated compile error' >&2\nexit 1\n",
            )

            result = run_host_tests.run_env("native", root, [str(pio_stub), "test"])

            self.assertFalse(result.passed, result.message)
            self.assertIn("pio test exit=1", result.message)

    def test_failing_pio_build_skips_the_binary_run_entirely(self):
        # A corrupt/partial binary from a failed link could hang; since
        # pio's exit code is already decisive on a build failure, the binary
        # must never be run at all (not run-with-diagnostic-timeout) -- the
        # verdict can't change either way.
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            build_dir = root / ".pio" / "build" / "native"
            build_dir.mkdir(parents=True)
            _write_stub_binary(
                build_dir, "program",
                "#!/bin/sh\necho '4714 Tests 0 Failures 0 Ignored'\nexit 0\n",
            )
            pio_stub = _write_stub_binary(
                root, "pio-stub", "#!/bin/sh\nexit 1\n",
            )

            with mock.patch.object(run_host_tests, "evaluate_binary") as mock_evaluate:
                result = run_host_tests.run_env("native", root, [str(pio_stub), "test"])

            mock_evaluate.assert_not_called()
            self.assertFalse(result.passed, result.message)

    def test_clean_pio_build_and_passing_binary_is_pass(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            build_dir = root / ".pio" / "build" / "native"
            build_dir.mkdir(parents=True)
            _write_stub_binary(
                build_dir, "program",
                "#!/bin/sh\necho '2 Tests 0 Failures 0 Ignored'\nexit 0\n",
            )
            pio_stub = _write_stub_binary(root, "pio-stub", "#!/bin/sh\nexit 0\n")

            result = run_host_tests.run_env("native", root, [str(pio_stub), "test"])

            self.assertTrue(result.passed, result.message)

    def test_clean_pio_build_with_genuinely_failing_binary_is_fail(self):
        # The original B1-1137 shape: pio's build step is fine (exit 0), but
        # the tests themselves genuinely failed.
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            build_dir = root / ".pio" / "build" / "native"
            build_dir.mkdir(parents=True)
            _write_stub_binary(
                build_dir, "program",
                "#!/bin/sh\necho '2 Tests 1 Failures 0 Ignored'\nexit 1\n",
            )
            pio_stub = _write_stub_binary(root, "pio-stub", "#!/bin/sh\nexit 0\n")

            result = run_host_tests.run_env("native", root, [str(pio_stub), "test"])

            self.assertFalse(result.passed, result.message)


class TestRunEnvWiresBinaryTimeoutThrough(unittest.TestCase):
    """"Correct by inspection" is how the timeout ended up dead in the first
    place -- this asserts the actual value run_env() receives is the one
    evaluate_binary() gets called with, not just that the parameter exists."""

    def test_non_default_binary_timeout_reaches_evaluate_binary(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            build_dir = root / ".pio" / "build" / "native"
            build_dir.mkdir(parents=True)
            _write_stub_binary(
                build_dir, "program",
                "#!/bin/sh\necho '1 Tests 0 Failures 0 Ignored'\nexit 0\n",
            )
            pio_stub = _write_stub_binary(root, "pio-stub", "#!/bin/sh\nexit 0\n")

            with mock.patch.object(
                run_host_tests, "evaluate_binary",
                wraps=run_host_tests.evaluate_binary,
            ) as spy_evaluate:
                run_host_tests.run_env("native", root, [str(pio_stub), "test"], binary_timeout=12.5)

            spy_evaluate.assert_called_once()
            _, kwargs = spy_evaluate.call_args
            self.assertEqual(kwargs.get("timeout"), 12.5)


class TestBinaryPathResolution(unittest.TestCase):
    def test_prefers_existing_program_binary(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            build_dir = root / ".pio" / "build" / "native"
            build_dir.mkdir(parents=True)
            expected = _write_stub_binary(build_dir, "program", "#!/bin/sh\nexit 0\n")
            self.assertEqual(run_host_tests._binary_path(root, "native"), expected)

    def test_falls_back_to_conventional_name_when_absent(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            expected = root / ".pio" / "build" / "native" / "program"
            self.assertEqual(run_host_tests._binary_path(root, "native"), expected)


if __name__ == "__main__":
    unittest.main()
