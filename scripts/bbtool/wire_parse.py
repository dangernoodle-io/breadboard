"""AUTOWIRE marker parser (decision #735): pure text-in/entries-out parsing of
`// bbtool:init` grep-marker comments from a component's public header text.

Grammar (one marker per comment line, order of key=value tokens is free):

    // bbtool:init tier=early|pre_http|regular [order=N] fn=<sym>
                    [server=true] [provides=k,..] [requires=k,..] [consumes=k]
                    [ctx=<expr>] [args=<raw C argument text, whitespace-free>]
                    [component=<name>]

`component=<name>` (B1-1275) names the component that must be composed for
this entry's `fn=` to link -- meaningful ONLY on a marker collected from a
CONSUMER MANIFEST (`--consumer-manifest`, see `commands.wire.
collect_manifest_entries`): a manifest entry deliberately has no owning
component of its own (unlike a marker grepped from a component header, whose
owning component is implicit), so it has no other way to tell codegen which
component's REQUIRES/PRIV_REQUIRES closure to pull in for a call it emits
into that component. `commands.codegen.run` reads this field off manifest
entries BEFORE composition resolution and folds the named component(s) into
the same `boards.resolve_transitive` closure walk `[capability.*].components`
feeds -- so `component=bb_wifi_prov` pulls in exactly what listing
`bb_wifi_prov` in a capability's `components` list would (its transitive
REQUIRES/PRIV_REQUIRES included). An unknown/misspelled name is a hard error
naming both the component and the manifest file (see codegen.py), never a
silent skip.

This parser accepts `component=` on ANY marker (it has no notion of manifest
vs. component-header context -- that's a property of which collector
(`collect_entries` vs `collect_manifest_entries`) parsed the text, not of the
marker text itself). `component=` on a marker grepped from a COMPONENT
HEADER (via `collect_entries`) is therefore rejected as a hard error one
layer up, in `commands.wire.collect_entries` -- a component header already
has an owning component by construction, so letting one component's marker
silently pull in ANOTHER component via `component=` would be an unreviewed,
un-fenced composition edge (exactly the kind of implicit glue the DI-legacy
fence exists to prevent) with no `[capability.*]`/manifest declaration
anywhere naming it.

`args=<...>` (parameterized-call grammar) lets a marker's `fn` be called with
an explicit, hand-authored C argument list instead of the zero-arg convention
-- e.g. `args=bb_wifi_prov_default_form_get(),1,NULL` renders
`bb_wifi_prov_autoinit(bb_wifi_prov_default_form_get(),1,NULL);` (see
`commands.wire._emit_args_call`). The value is spliced VERBATIM into the
generated call -- it is NEVER split on its internal commas (unlike
`provides=`/`requires=`, whose commas ARE list separators): `args=` is
captured as a single raw token up to the next whitespace, precisely because
its own commas are payload, not delimiters. This is why `args=` values MUST
be whitespace-free -- the marker line is itself whitespace-tokenized (see
`_parse_marker_line` below), so any space inside an `args=` value would be
parsed as the start of a new, unrelated token. Nothing here validates the
symbols/types inside `args=` -- the C compiler is the real validator, exactly
like the rest of this grep-time DSL (see the "Known limitation" paragraph
below). `args=` is mutually exclusive with `server=true` (the http-handle
path already supplies its own argument) and with `consumes=` (the
setter-injection path already supplies its own `(symbol, ctx)` argument
pair).

`ctx=<expr>` (B1-1045 PR-1) names a C expression passed as the second
argument to a `consumes=` setter call (see `commands.wire._emit_consumes_call`)
-- the setter's own producer-supplied ctx pointer, threaded through to every
invocation of the provider symbol it registers. `ctx=` REQUIRES `consumes=`
on the same marker line (a hard ParseError otherwise -- ctx is meaningless
without a setter to pass it to). Omitting `ctx=` on a `consumes=` marker is
still valid: the setter call's second argument defaults to the literal
`NULL`.

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

_KNOWN_KEYS = {
    "tier", "order", "fn", "server", "provides", "requires", "consumes", "ctx", "args",
    "component",
}
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
    ctx: "str | None" = None
    args: "str | None" = None
    component: "str | None" = None
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

    ctx = fields.get("ctx")
    if ctx is not None and consumes is None:
        raise ParseError(
            f"{src_file}:{lineno}: 'ctx=' requires 'consumes=' on the same "
            f"marker (ctx is meaningless without a setter to pass it to)"
        )

    # args= is captured VERBATIM -- it must never go through _split_csv
    # (its own commas are payload, not list separators). The value is
    # already whitespace-free by construction: the marker line itself is
    # whitespace-tokenized above, so an embedded space would have already
    # split into a separate, unrelated (likely "unknown key") token before
    # reaching this point -- there is no extra validation to do here beyond
    # the mutual-exclusion checks.
    args = fields.get("args")
    if args is not None and server:
        raise ParseError(
            f"{src_file}:{lineno}: 'args=' and 'server=true' are mutually "
            f"exclusive (args supplies its own argument list, server "
            f"supplies the captured http handle as the sole argument)"
        )
    if args is not None and consumes is not None:
        raise ParseError(
            f"{src_file}:{lineno}: 'args=' and 'consumes=' are mutually "
            f"exclusive (args supplies its own argument list, consumes "
            f"supplies the setter's (symbol, ctx) argument pair)"
        )

    # component= is captured verbatim, same as fn= -- meaning/validity is
    # context-dependent (manifest vs. component-header marker), decided by
    # the caller (see collect_manifest_entries/collect_entries in
    # commands.wire), never here.
    component = fields.get("component")

    return InitEntry(
        tier=tier,
        fn=fn,
        order=order,
        server=server,
        provides=provides,
        requires=requires,
        consumes=consumes,
        ctx=ctx,
        args=args,
        component=component,
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
