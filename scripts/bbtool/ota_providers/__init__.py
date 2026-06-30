"""OTA provider plugin registry and loader."""
from __future__ import annotations

import importlib
import importlib.util
import os
import pkgutil
import sys
import warnings
from typing import Optional

from .base import Provider


class ProviderRegistry:
    """Registry mapping provider names to Provider subclasses."""

    def __init__(self) -> None:
        self._providers: dict[str, type] = {}

    def register(self, name: str, cls: type) -> None:
        self._providers[name] = cls

    def get(self, name: str) -> Optional[type]:
        return self._providers.get(name)

    def names(self) -> list[str]:
        return sorted(self._providers.keys())


def _discover_builtins(registry: ProviderRegistry) -> None:
    """Auto-discover built-in provider modules in this package."""
    package_path = os.path.dirname(__file__)
    package_name = __name__
    for finder, module_name, _ispkg in pkgutil.iter_modules([package_path]):
        if module_name in ("__init__", "base"):
            continue
        full_name = f"{package_name}.{module_name}"
        try:
            mod = importlib.import_module(full_name)
            if hasattr(mod, "register"):
                mod.register(registry)
        except Exception as exc:
            warnings.warn(f"ota_providers: failed to load built-in {module_name}: {exc}")


def load_external(paths, config_dir: str, registry: ProviderRegistry) -> None:
    """Load external provider .py files; warn and continue on failure."""
    for p in paths:
        plugin_path = os.path.join(config_dir, p) if not os.path.isabs(p) else p
        try:
            spec = importlib.util.spec_from_file_location("_bbtool_ota_provider", plugin_path)
            if spec is None or spec.loader is None:
                raise ImportError(f"cannot load spec from {plugin_path}")
            mod = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(mod)
            if hasattr(mod, "register"):
                mod.register(registry)
        except Exception as exc:
            print(f"bbtool: ota provider load failed ({plugin_path}): {exc}",
                  file=sys.stderr)


def build_registry(config: Optional[dict] = None, config_dir: Optional[str] = None) -> ProviderRegistry:
    """Build a ProviderRegistry with built-in discovery + optional external providers.

    External provider paths are read from config under [ota][providers][paths]
    or [ota.providers][paths].
    """
    registry = ProviderRegistry()
    _discover_builtins(registry)

    if config and config_dir:
        # Support both [ota.providers] and [ota][providers] TOML table forms
        ota_cfg = config.get("ota", {})
        providers_cfg = ota_cfg.get("providers", {})
        paths = providers_cfg.get("paths", [])
        if paths:
            load_external(paths, config_dir, registry)

    return registry
