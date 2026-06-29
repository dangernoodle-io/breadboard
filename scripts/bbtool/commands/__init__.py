"""First-party plugin list — imported by cli.py to load built-ins onto the bus."""
from commands import lint as _lint
from commands import version as _version
from commands import embed as _embed
from commands import gen_site as _gen_site
from commands import fetch as _fetch
from commands import elf as _elf
from commands import decode as _decode

FIRST_PARTY = [_lint, _version, _embed, _gen_site, _fetch, _elf, _decode]
