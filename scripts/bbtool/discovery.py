"""discovery.py — indexed single-pass SSOT for component name -> directory
discovery (B1-979). Replaces the 4 hand-rolled path-position encodings
previously scattered across `boards.py`, `commands/wire.py`, and
`commands/lint.py` with one build+query pair.

Component identity — the leaf rule (PR1 of the depth-agnostic-layout lane):
  - On the `components/` side, a component is the INNERMOST directory
    (walking down from `components/`) that directly contains a
    `CMakeLists.txt`. An intermediate/group directory with no
    `CMakeLists.txt` of its own is never itself a component — this makes
    today's flat `components/<name>/CMakeLists.txt` and a future grouped
    `components/<group>/<name>/CMakeLists.txt` both resolve to component
    name `<name>`, at any nesting depth. Once a directory is accepted as a
    leaf, its subtree is not walked further (components never nest inside
    components), and a `.`-prefixed directory name is skipped entirely.
  - On the `platform/<plat>/` side, discovery stays a one-level `iterdir()`
    with NO `CMakeLists.txt` check, unchanged from pre-leaf-rule behavior.
    In-tree investigation (this PR) found that of ~140 `platform/<plat>/
    <name>/` directories, only 2 (`platform/espidf/bb_ota_push`,
    `platform/espidf/bb_queue_espidf`) carry their own `CMakeLists.txt` —
    the rest build as part of the owning component's CMakeLists via
    REQUIRES/PRIV_REQUIRES + platform-guarded sources, not a separate CMake
    target. Requiring a `CMakeLists.txt` here would silently drop nearly
    every platform-side entry, so the leaf rule applies to `components/`
    only for now; the two exceptions already work fine under the unchanged
    one-level scan (they simply happen to also have a `CMakeLists.txt` at
    that same depth-1 position). Nested platform-side layouts are out of
    scope for this PR.

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
        # Precomputed (abs_dir, rel_dir, name) triples for `owner_of_path`'s
        # longest-prefix match, sorted deepest-first (most path `parts`) so
        # a leaf component's own dir is matched before any shallower
        # indexed dir could (there never IS a shallower indexed dir on the
        # same branch under the leaf rule -- group dirs are never
        # components -- but sorting deepest-first keeps this correct even
        # if that invariant is ever relaxed). `rel_dir` is the dir
        # expressed relative to its OWNING entry's own root (`None` if that
        # relation doesn't hold, which never happens in practice since
        # every dir is scanned from under its own root) -- used to resolve
        # already-root-relative `path` arguments without requiring an
        # absolute root prefix.
        dirs = []
        for name, e in entries.items():
            root_path = Path(e.root)
            candidates = list(e.platform_dirs.values())
            if e.component_dir is not None:
                candidates.append(e.component_dir)
            for d in candidates:
                try:
                    rel_dir = d.relative_to(root_path)
                except ValueError:
                    rel_dir = None
                dirs.append((d, rel_dir, name))
        dirs.sort(key=lambda t: len(t[0].parts), reverse=True)
        self._dirs_by_depth = dirs

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
        one of this index's roots — or already root-relative) via
        LONGEST-PREFIX match against this index's actually-discovered
        component/platform directories — not a fixed path-position
        encoding, so it stays correct under any nesting depth on the
        `components/` side (a file under `components/<group>/<name>/...`
        attributes to the leaf component `<name>`, never the group).
        Returns `None` for a path that isn't under any indexed directory —
        never raises.

        An absolute `path` is canonicalized via `os.path.realpath` before
        matching (symmetric with how every indexed directory and root is
        already canonical, since `build_index()` canonicalizes every
        root) — a caller passing an absolute path built from an
        un-normalized root spelling (e.g. a symlink alias) still resolves
        correctly. A relative `path` is matched against each indexed
        directory expressed relative to ITS OWN owning root (`entry.root`)
        — the already-root-relative convention every pre-existing caller
        relies on — never against an absolute root prefix."""
        p = Path(path)
        if p.is_absolute():
            p = Path(os.path.realpath(str(p)))
            for abs_dir, _rel_dir, name in self._dirs_by_depth:
                try:
                    p.relative_to(abs_dir)
                    return name
                except ValueError:
                    continue
            return None

        for _abs_dir, rel_dir, name in self._dirs_by_depth:
            if rel_dir is None:
                continue
            try:
                p.relative_to(rel_dir)
                return name
            except ValueError:
                continue
        return None


def _leaf_component_dirs(base: Path, _ancestors: frozenset = frozenset()):
    """Yield each leaf-component directory under `base`, depth-first: the
    innermost directory (per branch, walking down from `base`) that
    directly contains a `CMakeLists.txt` — the "leaf rule". A directory is
    never descended into once accepted as a leaf (components don't nest
    inside components), so a stray `CMakeLists.txt` somewhere under an
    already-accepted leaf (e.g. a vendored subproject) can never surface as
    a second, spurious component. A directory whose name starts with `.`
    is skipped entirely (and not descended into), matching this repo's
    general excluded/hidden-dir convention. Yields nothing for a `base`
    that doesn't exist or isn't a directory.

    `_ancestors` (internal, realpath-keyed) guards against a symlink cycle:
    `Path.is_dir()`/`iterdir()` follow symlinks by default, so an unguarded
    recursive walk over a symlinked directory cycle under `components/`
    would recurse until Python's default recursion limit trips a
    `RecursionError` — a risk this walk introduces that the old
    single-level `iterdir()` it replaced could not structurally hit.

    Deliberately ANCESTOR-SCOPED (the current recursion path, passed down
    as a new `frozenset` per call — never a single mutated/shared set):
    skip a directory only when its realpath is already among its OWN
    ancestors on THIS branch — a genuine cycle. A global "ever visited"
    set would also silently suppress a non-cyclic symlink DIAMOND — e.g.
    two sibling dirs `components/alias_one` and `components/alias_two`
    both pointing at the same real group dir — discarding the second
    alias's subtree entirely and turning what should be a loud
    `CollisionError` (the SAME leaf component reachable via two different
    on-disk paths) into a silent single-pick. Each sibling branch starts
    fresh from its parent's ancestor set, so a diamond is walked down BOTH
    aliases and the duplicate is still discovered; only an actual
    self/mutual cycle (a realpath repeating within one branch's own
    ancestor chain) is skipped. Do not "simplify" this back to a shared/
    mutated set — that reintroduces the diamond-suppression bug."""
    if not base.is_dir():
        return
    real = os.path.realpath(str(base))
    if real in _ancestors:
        return
    child_ancestors = _ancestors | {real}
    for child in sorted(base.iterdir()):
        if not child.is_dir() or child.name.startswith("."):
            continue
        if (child / "CMakeLists.txt").is_file():
            yield child
        else:
            yield from _leaf_component_dirs(child, child_ancestors)


def _scan_root(root: str, platforms) -> Dict[str, ComponentEntry]:
    """Single-root scan. `components/` side: the leaf rule
    (`_leaf_component_dirs`) — a component's NAME is its leaf directory's
    own name, at any nesting depth, never a group/intermediate directory's
    name. Two distinct leaf directories resolving to the same name within
    this one root (e.g. `components/a/bb_dup/` and `components/b/bb_dup/`)
    is a `CollisionError`, raised immediately — mirrors the cross-root
    collision `build_index()` already enforces, just one level earlier
    (within-root, same identity rule: one name, one directory).
    `platform/{platforms}/` side: unchanged one-level `iterdir()`, no
    `CMakeLists.txt` check — see the module docstring's "leaf rule" section
    for why."""
    root_p = Path(root)
    raw: Dict[str, Dict[str, object]] = {}

    comp_root = root_p / "components"
    comp_seen: Dict[str, Path] = {}
    for leaf in _leaf_component_dirs(comp_root):
        if leaf.name in comp_seen:
            raise CollisionError(
                f"component name collision within root '{root}': "
                f"'{leaf.name}' in {sorted([str(comp_seen[leaf.name]), str(leaf)])}"
            )
        comp_seen[leaf.name] = leaf
        raw.setdefault(leaf.name, {"component_dir": None, "platform_dirs": {}})
        raw[leaf.name]["component_dir"] = leaf

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
    already give distinct cache keys). NOTE: "static within one tool
    invocation" is an invariant of today's callers (every CLI subcommand
    scans, acts, and exits — one process, one static tree), not something
    this cache enforces itself; a future single-process command that WRITES
    a new component dir and then re-queries `build_index()` in the same
    invocation would silently see the pre-write, now-stale index unless it
    calls `cache_clear()` first."""
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
