"""Shared `fence` CLI invocation helper for every `test_fence*.py` family
test module (B1-1128). Consolidates what used to be 6 near-identical
copies of `_run_fence_cli` into one place — CLAUDE.md's reuse rule
triggers extraction on the SECOND hand-rolled instance of a shared idiom,
and this idiom had grown to six.

Clears `discovery.build_index`'s cache before every invocation.
`build_index()` is memoized per (canonicalized-roots, platforms) for the
lifetime of the process — safe in real usage (one real CLI invocation is
one process against a static, unmutated tree), but NOT safe here: these
tests mutate the SAME tmp root across several calls within one long-lived
test process, so a stale cached index would silently hide a
just-added/removed component or file on the next call (mirrors
`discovery.py`'s own documented test convention).

This clear deliberately lives here, at the test-invocation layer, and NOT
inside `fence_cmd.run()` (B1-1128 review finding): `bbtool` dispatches
exactly one subcommand per process (see `scripts/bbtool/cli.py`), so in
every real invocation the cache is already empty on entry and a clear
inside `run()` is a permanent production no-op — while still globally
discarding any warm cache a future composed caller (invoking `run()`
more than once in-process) might legitimately want to keep.
"""
import argparse
import contextlib
import io

from commands import fence_cmd
from discovery import build_index


def run_fence_cli(root: str, family=None, update_baseline: bool = False, seed=None, approve=None) -> tuple:
    build_index.cache_clear()
    args = argparse.Namespace(
        root=root, family=family, update_baseline=update_baseline, seed=seed, approve=approve
    )
    stdout, stderr = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
        rc = fence_cmd.run(args)
    return rc, stdout.getvalue(), stderr.getvalue()
