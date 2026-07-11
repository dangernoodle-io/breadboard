"""AUTOWIRE marker parser (decision #735): pure text-in/entries-out parsing of
`// bbtool:init` grep-marker comments from a component's public header text.

Grammar (one marker per comment line, order of key=value tokens is free):

    // bbtool:init tier=early|pre_http|regular [order=N] fn=<sym>
                    [server=true] [provides=k,..] [requires=k,..] [consumes=k]

No preprocessor is involved — bbtool greps the raw header text. `tier` and
`fn` are required; everything else is optional. Any malformed marker (missing
tier/fn, unknown tier, unknown key, non-integer order, junk after `=`) is a
hard ParseError naming the offending file:line — never a silent skip, mirrors
boards.py's ManifestError posture.

`consumes=<key>` marks `fn` as a void-returning SETTER to be called with a
provider symbol at codegen time (see `commands.wire.render_source`) — never
the http-handle path, so it is mutually exclusive with `server=true`. Unlike
`requires=`, an unmatched `consumes` key is NOT a hard error: it is a signal
to silently drop the entry from generated output (see `ProvidesEntry` /
`_value_providers` below).

A second, sibling marker declares the provider side of that binding — a pure
name/symbol declaration, never an InitEntry, never entered into the tier
loop:

    // bbtool:provides key=<key> symbol=<symbol>

**Known limitation:** the marker scan is a naive `startswith("// bbtool:init")`
/ `startswith("// bbtool:provides")` line match (see `parse_markers` /
`parse_provides_markers` below) — it has no awareness of C string/comment
context, so the literal text appearing inside a string literal or a block
comment would also be treated as a marker. This is accepted for now (headers
are hand-authored, not adversarial input); a real tokenizer would be overkill
for a grep-marker DSL.
"""
from __future__ import annotations
from dataclasses import dataclass, field
from typing import List, Tuple

TIERS = ("early", "pre_http", "regular")
TIER_RANK = {name: i for i, name in enumerate(TIERS)}

_MARKER_PREFIX = "// bbtool:init"
_PROVIDES_PREFIX = "// bbtool:provides"

_KNOWN_KEYS = {"tier", "order", "fn", "server", "provides", "requires", "consumes"}
_PROVIDES_KNOWN_KEYS = {"key", "symbol"}


class ParseError(Exception):
    """Raised for any malformed `// bbtool:init` / `// bbtool:provides`
    marker — always a hard error, never a silent skip."""


@dataclass(frozen=True)
class InitEntry:
    tier: str
    fn: str
    order: "int | None" = None
    server: bool = False
    provides: Tuple[str, ...] = field(default_factory=tuple)
    requires: Tuple[str, ...] = field(default_factory=tuple)
    consumes: "str | None" = None
    src_file: str = "<string>"
    src_line: int = 0


@dataclass(frozen=True)
class ProvidesEntry:
    """A `// bbtool:provides key=<key> symbol=<symbol>` declaration — a
    name/symbol binding only. NEVER an InitEntry: no tier, no fn-call, never
    enters wire_graph.topo_sort or the tier-emission loop. Consumed only by
    the `consumes=` setter-injection path in `commands.wire.render_source`,
    which looks up an entry's `consumes` key in the map of these records to
    decide whether (and with what symbol) to emit a setter call."""
    key: str
    symbol: str
    src_file: str = "<string>"
    src_line: int = 0


def _split_csv(value: str) -> Tuple[str, ...]:
    return tuple(v.strip() for v in value.split(",") if v.strip())


def _parse_marker_line(line: str, lineno: int, src_file: str) -> InitEntry:
    rest = line.strip()[len(_MARKER_PREFIX):].strip()
    if not rest:
        raise ParseError(f"{src_file}:{lineno}: empty '// bbtool:init' marker")

    fields = {}
    for token in rest.split():
        if "=" not in token:
            raise ParseError(
                f"{src_file}:{lineno}: malformed token '{token}' "
                f"(expected key=value)"
            )
        key, _, value = token.partition("=")
        key = key.strip()
        value = value.strip()
        if not key or not value:
            raise ParseError(f"{src_file}:{lineno}: malformed token '{token}'")
        if key not in _KNOWN_KEYS:
            raise ParseError(f"{src_file}:{lineno}: unknown key '{key}'")
        if key in fields:
            raise ParseError(f"{src_file}:{lineno}: duplicate key '{key}'")
        fields[key] = value

    tier = fields.get("tier")
    if tier is None:
        raise ParseError(f"{src_file}:{lineno}: missing required 'tier='")
    if tier not in TIERS:
        raise ParseError(
            f"{src_file}:{lineno}: unknown tier '{tier}' (expected one of "
            f"{', '.join(TIERS)})"
        )

    fn = fields.get("fn")
    if fn is None:
        raise ParseError(f"{src_file}:{lineno}: missing required 'fn='")

    order = None
    if "order" in fields:
        raw_order = fields["order"]
        try:
            order = int(raw_order)
        except ValueError:
            raise ParseError(
                f"{src_file}:{lineno}: 'order=' must be an integer, got '{raw_order}'"
            )

    server = False
    if "server" in fields:
        raw_server = fields["server"]
        if raw_server != "true":
            raise ParseError(
                f"{src_file}:{lineno}: 'server=' must be 'true', got '{raw_server}'"
            )
        server = True

    provides = _split_csv(fields["provides"]) if "provides" in fields else ()
    requires = _split_csv(fields["requires"]) if "requires" in fields else ()

    consumes = fields.get("consumes")
    if consumes is not None and "," in consumes:
        raise ParseError(
            f"{src_file}:{lineno}: 'consumes=' accepts a single key, got "
            f"'{consumes}' (commas not allowed — unlike provides=/requires=, "
            f"consumes is not a csv list)"
        )
    if consumes is not None and server:
        raise ParseError(
            f"{src_file}:{lineno}: 'consumes=' and 'server=true' are mutually "
            f"exclusive (consumes is for the setter-injection path, server is "
            f"for the http-handle-threading path)"
        )

    return InitEntry(
        tier=tier,
        fn=fn,
        order=order,
        server=server,
        provides=provides,
        requires=requires,
        consumes=consumes,
        src_file=src_file,
        src_line=lineno,
    )


def parse_markers(header_text: str, src_file: str = "<string>") -> List[InitEntry]:
    """Parse every `// bbtool:init ...` marker line out of `header_text`,
    in file order. `src_file` is carried through onto each `InitEntry` for
    error messages / provenance only (parsing itself is pure over the text)."""
    entries: List[InitEntry] = []
    for lineno, line in enumerate(header_text.splitlines(), start=1):
        stripped = line.strip()
        if stripped.startswith(_MARKER_PREFIX):
            entries.append(_parse_marker_line(line, lineno, src_file))
    return entries


def _parse_provides_marker_line(line: str, lineno: int, src_file: str) -> ProvidesEntry:
    rest = line.strip()[len(_PROVIDES_PREFIX):].strip()
    if not rest:
        raise ParseError(f"{src_file}:{lineno}: empty '// bbtool:provides' marker")

    fields = {}
    for token in rest.split():
        if "=" not in token:
            raise ParseError(
                f"{src_file}:{lineno}: malformed token '{token}' "
                f"(expected key=value)"
            )
        key, _, value = token.partition("=")
        key = key.strip()
        value = value.strip()
        if not key or not value:
            raise ParseError(f"{src_file}:{lineno}: malformed token '{token}'")
        if key not in _PROVIDES_KNOWN_KEYS:
            raise ParseError(f"{src_file}:{lineno}: unknown key '{key}'")
        if key in fields:
            raise ParseError(f"{src_file}:{lineno}: duplicate key '{key}'")
        fields[key] = value

    key = fields.get("key")
    if key is None:
        raise ParseError(f"{src_file}:{lineno}: missing required 'key='")

    symbol = fields.get("symbol")
    if symbol is None:
        raise ParseError(f"{src_file}:{lineno}: missing required 'symbol='")

    return ProvidesEntry(key=key, symbol=symbol, src_file=src_file, src_line=lineno)


def parse_provides_markers(header_text: str, src_file: str = "<string>") -> List[ProvidesEntry]:
    """Parse every `// bbtool:provides ...` declaration line out of
    `header_text`, in file order. These are name/symbol bindings only — never
    InitEntry records, never fed into wire_graph.topo_sort."""
    entries: List[ProvidesEntry] = []
    for lineno, line in enumerate(header_text.splitlines(), start=1):
        stripped = line.strip()
        if stripped.startswith(_PROVIDES_PREFIX):
            entries.append(_parse_provides_marker_line(line, lineno, src_file))
    return entries
