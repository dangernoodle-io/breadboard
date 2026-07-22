#!/usr/bin/env python3
"""Authoritative host-test pass/fail verdict (B1-1137).

`pio test`'s own asyncio test-output reader
(platformio/test/runners/readers/native.py, PlatformIO 6.1.19 -- unpinned in
CI, B1-1141) has two independent upstream defects that can turn a genuine
test failure green:

  Bug A -- exit code misread as a signal. `raise_for_status()` calls
    `signal.Signals(abs(return_code))` on ANY nonzero return code without
    checking whether the child actually died by signal (negative returncode)
    or exited normally via `exit(N)` (positive). Unity's `main()` does
    `return UNITY_END()`, i.e. it returns the FAILURE COUNT as the process
    exit status -- one failing test exits 1, which PlatformIO reinterprets as
    "the process received signal SIGHUP" instead of "one test failed".

  Bug B -- lost trailing output. Under `pio test`, the child's stdout is a
    pipe, so libc fully buffers it and the tail (including Unity's own
    summary line) is flushed only at process exit. PlatformIO's asyncio
    reader ends the run on `process_exited()`, which races the final
    `pipe_data_received()` delivery of that buffered tail -- the summary line
    can be (and was, reproducibly) dropped from the captured output.

Neither defect lives in PlatformIO's BUILD step, only in its
test-EXECUTION/reporting reader -- so this script uses `pio test` to build
and print a human-readable per-case report (its TEST verdict is never
trusted), then determines pass/fail itself from a direct, synchronous
`subprocess.run()` of the already-compiled Unity binary: no asyncio, no pipe
race, nothing to misread. `subprocess.run()` blocks until the child's stdout
pipe is closed (EOF), so the full buffered tail -- including the summary
line -- is always captured. The binary verdict is cross-checked against
Unity's own "<N> Tests <M> Failures <P> Ignored" summary line; a missing
summary line, a zero-tests-executed summary, or a run that exceeds the
binary timeout are all treated as FAIL, never as an implicit pass.

Critically, `pio test`'s BUILD-step exit code IS trustworthy (Bugs A/B live
only in its test-execution reader) -- and a build failure (e.g. a syntax
error) leaves any PREVIOUSLY built binary on disk untouched, since SCons
stops before link. Trusting the binary check alone would silently re-run and
PASS on that stale binary while the tree doesn't even compile: a worse
false-green than the one this script exists to fix. So the overall verdict
per environment (see run_env()) requires BOTH `pio test`'s own exit code AND
the direct binary verdict to be clean.
"""
from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path
from typing import NamedTuple, Optional

SUMMARY_RE = re.compile(r"^(\d+) Tests (\d+) Failures (\d+) Ignored\s*$", re.MULTILINE)

# A hung test binary must not block a CI job forever with no diagnostic --
# bounded by a scoped TimeoutExpired instead of the job's own
# timeout-minutes (a far worse, undiagnosed failure mode).
DEFAULT_BINARY_TIMEOUT_S = 300.0


class HostTestResult(NamedTuple):
    passed: bool
    message: str


def evaluate_binary(binary_path: Path, timeout: Optional[float] = DEFAULT_BINARY_TIMEOUT_S) -> HostTestResult:
    """Run the compiled Unity binary directly and derive an authoritative
    verdict from its own exit code, cross-checked against its summary line.

    Never trusts PlatformIO's test reader (see module docstring) -- this is
    a plain synchronous subprocess run, immune to both Bug A (no signal
    reinterpretation of a positive exit code) and Bug B (subprocess.run()
    blocks for EOF, so the buffered tail is always captured whole).

    This function alone CANNOT see a build failure that left a stale,
    previously-built binary in place -- see run_env(), which additionally
    requires `pio test`'s own build-step exit code to be clean."""
    if not binary_path.is_file():
        return HostTestResult(False, f"test binary not found: {binary_path}")
    if not os.access(binary_path, os.X_OK):
        return HostTestResult(False, f"test binary is not executable: {binary_path}")

    try:
        proc = subprocess.run(
            [str(binary_path)],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=timeout,
        )
    except subprocess.TimeoutExpired:
        return HostTestResult(
            False, f"test binary did not complete within {timeout}s -- treated as a hang, not a pass"
        )
    output = proc.stdout

    match = SUMMARY_RE.search(output)
    if match is None:
        return HostTestResult(
            False,
            f"no Unity summary line found in output (exit={proc.returncode}) -- "
            "the run did not complete; this is never treated as a pass",
        )

    total, failures, ignored = (int(g) for g in match.groups())
    exit_clean = proc.returncode == 0
    summary_clean = failures == 0
    # A binary reporting zero executed tests (e.g. Unity misconfigured, a
    # test file silently dropped from the build) is exactly the "check
    # silently stopped covering something" shape this script exists to
    # catch -- never treat it as a pass.
    ran_something = total > 0

    if exit_clean and summary_clean and ran_something:
        return HostTestResult(True, f"{total} Tests {failures} Failures {ignored} Ignored")

    return HostTestResult(
        False,
        f"exit={proc.returncode} summary='{total} Tests {failures} Failures {ignored} Ignored' "
        "-- exit code, Unity summary, and/or zero-tests-executed report a failure",
    )


def _binary_path(project_root: Path, env: str) -> Path:
    build_dir = project_root / ".pio" / "build" / env
    for name in ("program", "program.exe"):
        candidate = build_dir / name
        if candidate.exists():
            return candidate
    # Fall back to the conventional name so the "not found" message below
    # points at the path we expected, even if the build never produced it.
    return build_dir / "program"


def run_env(
    env: str,
    project_root: Path,
    pio_cmd: list[str],
    binary_timeout: Optional[float] = DEFAULT_BINARY_TIMEOUT_S,
) -> HostTestResult:
    """The overall verdict requires BOTH pio's own build-step exit code AND
    the direct binary verdict to be clean -- neither alone is sufficient:

    - `pio test`'s TEST-execution/reporting verdict is untrustworthy (Bugs
      A/B, see module docstring), but its BUILD step is unaffected by either
      bug (they live only in the test-execution reader) -- a nonzero exit
      from a genuine compile failure is a perfectly reliable signal, and
      SCons stops before link on a compile error, leaving any PREVIOUSLY
      built binary untouched on disk. Trusting evaluate_binary() alone would
      silently re-run and PASS on that stale binary while the tree doesn't
      even compile -- a worse false-green than the one this script exists to
      fix.
    - evaluate_binary() alone cannot see that kind of stale-binary case (the
      binary it ran genuinely reports PASS); it exists to catch pio
      reporting green over a genuine test FAILURE, the original B1-1137 bug.

    So: pio_exit == 0 AND binary verdict == PASS. A false RED (pio exits
    nonzero on an otherwise-passing run) is an acceptable trade -- loud and
    investigated, unlike a false green."""
    print(f"==> pio test -e {env} (build + human-readable per-case report)", flush=True)
    # `pio test`'s TEST verdict is deliberately IGNORED (see module
    # docstring) -- but its exit CODE is still captured and required clean
    # below, because a build failure is a real, trustworthy nonzero exit.
    pio_proc = subprocess.run([*pio_cmd, "-e", env], cwd=project_root, check=False)

    if pio_proc.returncode != 0:
        # Build/run step already failed -- pio's exit code alone is decisive
        # (see module docstring: never overridden by a stale or otherwise-
        # clean binary), so skip the binary run entirely rather than pay up
        # to the full timeout evaluating a verdict that can't change it (a
        # corrupt/partial binary from a failed link could hang).
        message = f"pio test exit={pio_proc.returncode} (build/run step failed)"
        print(f"==> [{env}] FAIL: {message}", flush=True)
        return HostTestResult(False, message)

    binary = _binary_path(project_root, env)
    print(f"==> {binary} (authoritative verdict: direct synchronous run)", flush=True)
    binary_result = evaluate_binary(binary, timeout=binary_timeout)

    verdict = "PASS" if binary_result.passed else "FAIL"
    print(f"==> [{env}] {verdict}: {binary_result.message}", flush=True)
    return binary_result


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("envs", nargs="+", help="PlatformIO environment name(s) to test")
    parser.add_argument("--root", default=".", help="project root (default: cwd)")
    parser.add_argument("--pio", default="pio", help="pio executable (default: pio)")
    parser.add_argument(
        "--binary-timeout", type=float, default=DEFAULT_BINARY_TIMEOUT_S,
        help=f"seconds to allow the compiled test binary to run before treating it as a "
             f"hang (default: {DEFAULT_BINARY_TIMEOUT_S})",
    )
    args = parser.parse_args(argv)

    project_root = Path(args.root).resolve()
    pio_cmd = [args.pio, "test"]

    all_passed = True
    for env in args.envs:
        result = run_env(env, project_root, pio_cmd, binary_timeout=args.binary_timeout)
        all_passed = all_passed and result.passed

    if not all_passed:
        print("run_host_tests: at least one environment FAILED (see above)", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
