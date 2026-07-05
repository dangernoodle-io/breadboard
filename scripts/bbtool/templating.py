"""Minimal stdlib {{token}} + optional-region substitution engine. No Jinja,
no third-party template lib — two mechanisms only:

1. `{{var}}` — replaced by `str(vars[var])` in a single global pass. A token
   with no matching key is left untouched (callers that need to guarantee
   zero dangling tokens should validate the rendered output themselves).

2. Optional region:

       <!-- bbtool:optional:KEY -->
       ...body, may reference {{var}} tokens...
       <!-- /bbtool:optional:KEY -->

   If `optional_vars[KEY]` is a truthy dict, the region's *body* is kept
   (the marker comment lines themselves are stripped) and its `{{var}}`
   tokens are substituted from `vars` merged with that dict (region-local
   keys win on collision). If `optional_vars[KEY]` is missing/falsy, the
   entire region — both marker lines, the body, and one trailing newline —
   is dropped, leaving no dangling markup or tokens behind.

Determinism: pure string operations, no dict/set iteration-order leaks (the
body of a region is substituted with the caller-supplied `vars`/`optional_vars`
dicts as given — callers must pre-sort any generated list-like values before
building those dicts), no timestamps, no absolute paths. Rendering the same
inputs twice always yields byte-identical output.
"""
from __future__ import annotations
import re
from typing import Dict, Optional

_OPTIONAL_RE = re.compile(
    r'<!-- bbtool:optional:(?P<key>[A-Za-z0-9_-]+) -->\r?\n?'
    r'(?P<body>.*?)'
    r'<!-- /bbtool:optional:(?P=key) -->\r?\n?',
    re.DOTALL,
)

_TOKEN_RE = re.compile(r'\{\{(\w+)\}\}')


def _substitute(text: str, tokens: Dict[str, str]) -> str:
    def _sub(m: "re.Match") -> str:
        key = m.group(1)
        if key in tokens:
            return str(tokens[key])
        return m.group(0)
    return _TOKEN_RE.sub(_sub, text)


def render(
    template: str,
    vars: Dict[str, str],
    optional_vars: Optional[Dict[str, Optional[Dict[str, str]]]] = None,
) -> str:
    """Render `template` against `vars`, resolving any
    `bbtool:optional:KEY` regions per `optional_vars` (see module docstring).
    """
    optional_vars = optional_vars or {}

    def _region(m: "re.Match") -> str:
        key = m.group("key")
        region_vars = optional_vars.get(key)
        if not region_vars:
            return ""
        merged = dict(vars)
        merged.update(region_vars)
        return _substitute(m.group("body"), merged)

    resolved = _OPTIONAL_RE.sub(_region, template)
    return _substitute(resolved, vars)


def find_dangling_tokens(text: str) -> list:
    """Return every remaining `{{token}}` in `text` (should be empty after a
    correct render — used by callers/tests to assert no leftovers)."""
    return _TOKEN_RE.findall(text)
