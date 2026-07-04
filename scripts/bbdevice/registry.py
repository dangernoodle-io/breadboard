"""Command and rule registries + plugin API."""
from __future__ import annotations
import dataclasses
import sys
from typing import Any, Callable, Set

@dataclasses.dataclass
class Rule:
    id: str
    default_severity: str      # "error", "warn", "off"
    profiles: Set[str]         # e.g. {"all"} or {"library"}
    check: Callable            # fn(ctx) -> list[dict]  (violations)
    hint: str = ""

# Global registries
COMMANDS: dict[str, Any] = {}   # name -> command module
RULES: dict[str, Rule] = {}     # id -> Rule (for future lint-style commands)

class PluginAPI:
    """API surface exposed to plugin .py files via register(api)."""

    def add_rule(self, rule: Rule) -> None:
        if rule.id in RULES:
            print(f"bbdevice: plugin rule id collision '{rule.id}' — ignored", file=sys.stderr)
            return
        RULES[rule.id] = rule

    def add_command(self, name: str, module: Any) -> None:
        if name in COMMANDS:
            print(f"bbdevice: plugin command collision '{name}' — ignored", file=sys.stderr)
            return
        COMMANDS[name] = module
