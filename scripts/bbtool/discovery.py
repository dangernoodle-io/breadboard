"""discovery.py — indexed single-pass SSOT for component name -> directory
discovery (B1-979). Replaces the 4 hand-rolled path-position encodings
previously scattered across `boards.py`, `commands/wire.py`, and
`commands/lint.py` with one build+query pair.

Model (decided, B1-979):
  - Single global bare-NAME namespace: one component name maps to exactly
    one `ComponentEntry`. The index is keyed on name across the entire
    scanned tree (`components/` + `platform/{host,espidf,arduino}/`), never
    scoped per root.
  - Cross-tree collision (the SAME name discovered under more than one
    root) is a hard `CollisionError` raised at `build_index()` time —
    fail-loud, never a silent last-wins, never a warning. No
    override/shadow/allowlist mechanism (deferred — see B1-1086).
  - `build_index()` takes an ORDERED list of roots, and canonicalizes every
    root via `os.path.realpath` (resolving symlinks, `.`/`..` segments, and
    a trailing separator) before dedup/cache-keying/scanning — so two
    different spellings of the same physical directory always collapse to
    one root, never a spurious cross-root collision. This holds regardless
    of whether a caller pre-normalizes via `normalize_roots` (below) or
    calls `build_index()` directly. Multi-root feeding is wired end to end
    (B1-1084): `boards.py`, `composition.py`, and `commands/wire.py` all
    accept and resolve components against an ordered list of roots, each
    component read from its own owning root — see those modules' own
    docstrings for the call sites.

Determinism: no dict/set iteration-order leaks into any query result that
matters for build output (`.names()` returns a set, same contract
`boards.discover_components` always had); collision error text is sorted.
"""
from __future__ import annotations
import os
from dataclasses import dataclass
from functools import lru_cache
from pathlib import Path
from typing import Dict, List, Optional, Union

PLATFORMS = ("host", "espidf", "arduino")


class CollisionError(Exception):
    """Raised by `build_index()` when the same component name is discovered
    under more than one root — always a hard error, never a silent
    last-wins and never a warning. Names every offending name + root."""


@dataclass(frozen=True, slots=True)
class ComponentEntry:
    """One discovered component: its bare `name`, the `root` it was found
    under, its `components/<name>/` dir (`None` if it has no
    components-layer presence at all — a platform-only component), and its
    per-platform `platform/<plat>/<name>/` dirs (only platforms with an
    on-disk dir are present as keys)."""

    name: str
    root: str
    component_dir: Optional[Path]
    platform_dirs: Dict[str, Path]


class ComponentIndex:
    """Queryable name -> `ComponentEntry` index, built by `build_index()`.
    Every lookup is a plain dict get — an unknown name or path returns
    `None`, it never raises (collision is the only hard-error path, and
    that's raised once, at build time)."""

    def __init__(self, entries: Dict[str, ComponentEntry], roots: List[str]) -> None:
        self._entries = entries
        self._roots = [Path(r) for r in roots]

    def names(self) -> set:
        return set(self._entries.keys())

    def entry(self, name: str) -> Optional[ComponentEntry]:
        return self._entries.get(name)

    def component_dir(self, name: str) -> Optional[Path]:
        e = self._entries.get(name)
        return e.component_dir if e else None

    def platform_dir(self, name: str, platform: str) -> Optional[Path]:
        e = self._entries.get(name)
        return e.platform_dirs.get(platform) if e else None

    def owner_of_path(self, path) -> Optional[str]:
        """Derive the owning component name from a path (absolute — under
        one of this index's roots — or already root-relative), mirroring
        the directory convention `build_index()` scanned:
        `components/<name>/...` -> name; `platform/<plat>/<name>/...` ->
        name. Returns `None` for a path outside that convention, or for an
        owner name this index doesn't actually contain — never raises.

        An absolute `path` is canonicalized via `os.path.realpath` before
        matching against `self._roots` (also canonical, since `build_index()`
        canonicalizes every root) — a caller passing an absolute path built
        from an un-normalized root spelling (e.g. a symlink alias) still
        resolves correctly, symmetric with how roots themselves are
        canonicalized. A relative `path` is left untouched (there is nothing
        to resolve against)."""
        p = Path(path)
        if p.is_absolute():
            p = Path(os.path.realpath(str(p)))
        rel = p
        for root in self._roots:
            try:
                rel = p.relative_to(root)
                break
            except ValueError:
                continue

        parts = rel.parts
        if len(parts) >= 2 and parts[0] == "components":
            name = parts[1]
        elif len(parts) >= 3 and parts[0] == "platform":
            name = parts[2]
        else:
            return None
        return name if name in self._entries else None


def _scan_root(root: str, platforms) -> Dict[str, ComponentEntry]:
    """Single-root scan mirroring the pre-index `boards.discover_components`
    walk: every directory name under `components/` or
    `platform/{platforms}/`, recording the discovered dir(s) per name."""
    root_p = Path(root)
    raw: Dict[str, Dict[str, object]] = {}

    comp_root = root_p / "components"
    if comp_root.is_dir():
        for child in sorted(comp_root.iterdir()):
            if child.is_dir():
                raw.setdefault(child.name, {"component_dir": None, "platform_dirs": {}})
                raw[child.name]["component_dir"] = child

    for plat in platforms:
        plat_root = root_p / "platform" / plat
        if plat_root.is_dir():
            for child in sorted(plat_root.iterdir()):
                if child.is_dir():
                    entry = raw.setdefault(
                        child.name, {"component_dir": None, "platform_dirs": {}}
                    )
                    entry["platform_dirs"][plat] = child

    return {
        name: ComponentEntry(name, root, data["component_dir"], data["platform_dirs"])
        for name, data in raw.items()
    }


@lru_cache(maxsize=None)
def _build_index_cached(roots: tuple, platforms: tuple) -> ComponentIndex:
    """Memoized builder keyed on (roots, platforms) tuples — the component
    tree is static within a single bbtool/codegen invocation, so caching
    the scan across repeated `build_index()` calls (hot loops in
    `boards.derive_component`, `wire._component_headers`,
    `lint._emit_seam_owner_from_path`) is safe. Cleared via
    `build_index.cache_clear()`."""
    per_root: Dict[str, Dict[str, ComponentEntry]] = {
        root: _scan_root(root, platforms) for root in roots
    }

    # Collision check FIRST, over which root(s) each name was found under —
    # never build a name->entry merge that could contain a first-root-wins
    # pick that then gets discarded by the raise below. By the time we merge
    # (below), every name is already known to own exactly one root, so
    # `dict.update` per root is unambiguous — no override/shadow semantics,
    # just a merge of disjoint key sets.
    owner_roots: Dict[str, List[str]] = {}
    for root in roots:
        for name in per_root[root]:
            owner_roots.setdefault(name, []).append(root)

    collisions = {name: rs for name, rs in owner_roots.items() if len(rs) > 1}
    if collisions:
        detail = "; ".join(
            f"'{name}' in {roots_}" for name, roots_ in sorted(collisions.items())
        )
        raise CollisionError(f"component name collision across roots: {detail}")

    merged: Dict[str, ComponentEntry] = {}
    for root in roots:
        merged.update(per_root[root])

    return ComponentIndex(merged, list(roots))


def build_index(roots: List[str], platforms=PLATFORMS) -> ComponentIndex:
    """Scan each root in `roots` order (`components/` +
    `platform/{platforms}/`), merging into one name -> `ComponentEntry` map.
    A name discovered under more than one root is a hard `CollisionError`
    naming every offending name + root pair — raised only after every root
    has been scanned, so the error always lists every collision at once,
    not just the first. Single-root callers (every caller today) get
    discovery byte-identical to the pre-index `boards.discover_components`
    walk.

    Every root is canonicalized via `os.path.realpath` (resolving symlinks,
    `.`/`..` segments, and a trailing separator) BEFORE dedup/cache-keying —
    enforced here, not just by the `normalize_roots` convenience helper, so
    the symlink-collision fix holds by construction rather than being
    convention-based. A caller that bypasses `normalize_roots` and calls
    `build_index()` directly (e.g. `commands/lint.py`'s
    `_emit_seam_owner_from_path`) still gets one entry per physical
    directory — two different spellings of the same directory can never
    raise a spurious `CollisionError`, and always land in the same cache
    entry. `os.path.realpath` is idempotent, so a caller that already went
    through `normalize_roots` (which also canonicalizes) pays a second,
    harmless no-op syscall here rather than double-resolving anything.

    `roots` is deduped (order-preserving) AFTER canonicalization, so
    `build_index([root, root])` behaves as `[root]`, and two on-disk-
    identical spellings of one root also collapse to one — a duplicate (or
    aliased) root in the list never spuriously self-collides. Empty `roots`
    returns an empty index (no scan, no error).

    Memoized (B1-979 review #1) on the deduped, CANONICALIZED roots tuple +
    platforms tuple — safe because the on-disk component tree is static
    within one tool invocation, and canonicalizing before building the
    cache key is what guarantees two spellings of one root hit the same
    cache entry instead of scanning (and diffing) it twice. Call
    `build_index.cache_clear()` to invalidate (tests that build fresh temp
    trees should do this defensively, even though distinct temp paths
    already give distinct cache keys)."""
    deduped = list(dict.fromkeys(os.path.realpath(str(r)) for r in roots))
    return _build_index_cached(tuple(deduped), tuple(platforms))


build_index.cache_clear = _build_index_cached.cache_clear


def normalize_roots(roots: Union[str, "os.PathLike", List]) -> List[str]:
    """Accept either a single root (str/PathLike — every pre-B1-1084
    single-root caller's shape) or an ordered list of roots, and return a
    deduped (order-preserving), all-`str`, CANONICALIZED list. Centralizes
    the "roots or root" normalization every multi-root-aware caller in
    `boards.py`, `composition.py`, and `commands/wire.py` needs (B1-1084) so
    each of those modules can accept `roots` without hand-rolling this
    check, and so a bare single-root call site (untouched by B1-1084) keeps
    working with zero changes.

    Every root is canonicalized via `os.path.realpath` before dedup —
    resolves symlinks, `.`/`..` segments, and a trailing separator — so two
    different SPELLINGS of the same physical directory (a symlinked vendor
    tree/submodule, a `./`-relative form, a trailing slash) always dedupe to
    one root instead of raising a spurious `CollisionError` for every
    component under it. A single-root call whose root has no symlink/`.`/
    `..` components in its path (the common case for `os.getcwd()`-derived
    roots) realpaths to itself, so single-root discovery stays
    byte-identical.

    `build_index()` itself also canonicalizes (idempotently) every root it
    receives, so this dedup/canonicalization is enforced by construction
    even for a caller that skips `normalize_roots` and calls `build_index()`
    directly — this helper remains the convenient public entry point for
    accepting either a bare root or a list, not the sole place the
    symlink-collision fix lives."""
    if isinstance(roots, (str, os.PathLike)):
        roots = [roots]
    return list(dict.fromkeys(os.path.realpath(str(r)) for r in roots))
