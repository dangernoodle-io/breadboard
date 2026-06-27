"""End-to-end CLI tests for bbtool."""
import subprocess
import sys
import os
from pathlib import Path


class TestBBToolCLI:
    """Test bbtool CLI via subprocess invocations from the repo root."""

    @classmethod
    def setup_class(cls):
        """Set up test environment."""
        cls.repo_root = Path(__file__).resolve().parent.parent.parent.parent
        cls.bbtool_script = cls.repo_root / "scripts" / "bbtool.py"

    def run_bbtool(self, *args):
        """Run bbtool as a subprocess from repo root."""
        cmd = [sys.executable, str(self.bbtool_script)] + list(args)
        result = subprocess.run(
            cmd,
            cwd=str(self.repo_root),
            capture_output=True,
            text=True,
        )
        return result

    def test_lint_list(self):
        """Test: bbtool lint --list exits 0 and prints rules."""
        result = self.run_bbtool("lint", "--list")
        assert result.returncode == 0, f"Expected exit 0, got {result.returncode}\nstderr: {result.stderr}"
        # Check that output contains rule names
        assert "deprecated-http-send" in result.stdout
        assert "public-header-leak" in result.stdout
        assert "state-topic-post" in result.stdout
        assert "severity=" in result.stdout

    def test_help(self):
        """Test: bbtool --help exits 0 and lists subcommands."""
        result = self.run_bbtool("--help")
        assert result.returncode == 0, f"Expected exit 0, got {result.returncode}\nstderr: {result.stderr}"
        # Check for subcommand names in help text
        assert "lint" in result.stdout
        assert "version" in result.stdout
        assert "embed" in result.stdout
        assert "gen-site" in result.stdout

    def test_lint_with_root(self):
        """Test: bbtool lint --root . --profile library exits 0."""
        result = self.run_bbtool("lint", "--root", ".", "--profile", "library")
        assert result.returncode == 0, f"Expected exit 0, got {result.returncode}\nstderr: {result.stderr}"

    def test_version_emit(self):
        """Test: bbtool version --emit --consumer . --bb-dir . exits 0 and prints output."""
        result = self.run_bbtool("version", "--emit", "--consumer", ".", "--bb-dir", ".")
        assert result.returncode == 0, f"Expected exit 0, got {result.returncode}\nstderr: {result.stderr}"
        # Check that output is non-empty (a version string)
        assert len(result.stdout.strip()) > 0, f"Expected non-empty output, got: {result.stdout}"
