"""Shared library: config loading + plugin loader.

Generic bus helpers only — mirrors scripts/bbtool/core.py's shape. Device-
fleet-specific helpers (SettleConfig, SuiteContext, gate_enabled, etc.) land
in a later PR; see suite_framework.py for the placeholder.
"""
from __future__ import annotations
import importlib.util
import os
import sys
import tomllib
from pathlib import Path
from typing import Iterable, Optional


def load_config(config_path: Optional[str], root: str) -> dict:
    """Load bbdevice.toml. Discovery: --config → <root>/bbdevice.toml → {}."""
    if config_path:
        path = Path(config_path)
    else:
        path = Path(root) / "bbdevice.toml"
    if not path.exists():
        return {}
    try:
        with open(path, "rb") as fh:
            return tomllib.load(fh)
    except Exception as e:
        print(f"bbdevice: failed to load config {path}: {e}", file=sys.stderr)
        return {}


def load_plugins(paths: Iterable[str], config_dir: str, api: object) -> None:
    """Load plugin .py files; warn on failure, continue."""
    for p in paths:
        plugin_path = Path(p) if os.path.isabs(p) else Path(config_dir) / p
        try:
            spec = importlib.util.spec_from_file_location("_bbdevice_plugin", plugin_path)
            if spec is None or spec.loader is None:
                raise ImportError(f"cannot load spec from {plugin_path}")
            mod = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(mod)
            if hasattr(mod, "register"):
                mod.register(api)
        except Exception as e:
            print(f"bbdevice: plugin load failed ({plugin_path}): {e}", file=sys.stderr)
