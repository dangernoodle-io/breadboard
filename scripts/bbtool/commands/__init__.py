"""Register built-in commands into the COMMANDS registry."""
from registry import COMMANDS
from commands import lint as _lint

COMMANDS["lint"] = _lint
