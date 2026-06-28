"""Register built-in commands into the COMMANDS registry."""
from registry import COMMANDS
from commands import lint as _lint
from commands import version as _version
from commands import embed as _embed
from commands import gen_site as _gen_site
from commands import fetch as _fetch

COMMANDS["lint"] = _lint
COMMANDS["version"] = _version
COMMANDS["embed"] = _embed
COMMANDS["gen-site"] = _gen_site
COMMANDS["fetch"] = _fetch
