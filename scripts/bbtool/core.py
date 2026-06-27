"""Shared library: config loading, Context, plugin loader."""
from __future__ import annotations
import importlib.util
import os
import sys
import tomllib
from pathlib import Path
from typing import Generator, Iterable, Optional


def load_config(config_path: Optional[str], root: str) -> dict:
    """Load bbtool.toml. Discovery: --config → <root>/bbtool.toml → {}."""
    if config_path:
        path = Path(config_path)
    else:
        path = Path(root) / "bbtool.toml"
    if not path.exists():
        return {}
    try:
        with open(path, "rb") as fh:
            return tomllib.load(fh)
    except Exception as e:
        print(f"bbtool: failed to load config {path}: {e}", file=sys.stderr)
        return {}


class Context:
    """Shared context passed to rule check functions."""

    def __init__(self, root: str, config: dict) -> None:
        self.root = os.path.abspath(root)
        self.config = config

    def files(
        self,
        globs: Iterable[str],
        relative_to: Optional[str] = None,
        exclude_dirs: Optional[Iterable[str]] = None,
    ) -> Generator[Path, None, None]:
        """Yield paths matching any of the globs under root, skipping excluded dir names."""
        from fnmatch import fnmatch
        exclude_set = set(exclude_dirs or [])
        root = Path(self.root)
        seen = set()
        for pattern in globs:
            for path in root.glob(pattern):
                if path in seen:
                    continue
                # Check no path component is in exclude_set
                parts = path.relative_to(root).parts
                if any(p in exclude_set for p in parts):
                    continue
                seen.add(path)
                yield path

    def read(self, path: Path) -> str:
        """Read a file as UTF-8 with errors='replace'."""
        with open(path, encoding="utf-8", errors="replace") as fh:
            return fh.read()

    def violation(self, path: Path, line: int, detail: str = "") -> dict:
        return {"path": str(path), "line": line, "detail": detail}


def load_plugins(paths: Iterable[str], config_dir: str, api: object) -> None:
    """Load plugin .py files; warn on failure, continue."""
    for p in paths:
        plugin_path = Path(p) if os.path.isabs(p) else Path(config_dir) / p
        try:
            spec = importlib.util.spec_from_file_location("_bbtool_plugin", plugin_path)
            if spec is None or spec.loader is None:
                raise ImportError(f"cannot load spec from {plugin_path}")
            mod = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(mod)
            if hasattr(mod, "register"):
                mod.register(api)
        except Exception as e:
            print(f"bbtool: plugin load failed ({plugin_path}): {e}", file=sys.stderr)
