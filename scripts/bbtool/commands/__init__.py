"""Register built-in commands into the COMMANDS registry."""
from registry import COMMANDS
from commands import lint as _lint
from commands import version as _version

COMMANDS["lint"] = _lint
COMMANDS["version"] = _version
