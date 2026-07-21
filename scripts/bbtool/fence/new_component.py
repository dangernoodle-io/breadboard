"""new_component fence family — the "no unauthorized components" guardrail.

Unlike every other fence family (drain-only ratchets against a legacy or
hand-rolled surface), this one guards a CREATION event: a brand-new
`components/<name>/` directory. Per the component-creation policy
(CLAUDE.md "Component creation" + wiki design/Component-Taxonomy#when-to-
create-a-new-component, KB #402), extending an existing component is the
default; a genuinely new component requires a distinct dependency, a real
consumer, and reviewed design sign-off — never a speculative/ad-hoc add.

Marker type scanned: `component` — one marker per LEAF component directory
under `components/` (per `discovery.py`'s leaf rule: the innermost directory,
walking down from `components/`, that directly contains a `CMakeLists.txt`;
an intermediate/group directory with no `CMakeLists.txt` of its own is never
itself a component). Identity is the component name alone (`id`), so a
directory rename — or a relocation to a different nesting depth, as long as
the leaf directory's own name is unchanged — is NOT treated as a new
component; identity is name-keyed per the same rename-stability convention
every other family uses (see `_base.diff`). The marker's `path` field
reflects wherever the leaf directory actually lives (`components/<name>` on
today's flat tree, `components/<group>/<name>` under a future grouped
layout) — `diff()` pairs by exact path first, so on today's tree this is
byte-identical to the old fixed `components/<name>` convention; a pure
relocation to a different depth is identity-stable (same `id`, no baseline
change needed), same as any other family's rename tolerance.

LATENT GAP: requiring a `CMakeLists.txt` to count as a leaf component means
a stray non-`EXCLUDE_DIRS` directory under `components/` that has no
`CMakeLists.txt` of its own is now silently NOT fenced — previously
(directly-under-`components/`, no CMakeLists.txt check) it would have
tripped the guardrail as an unapproved new component. A no-op on today's
tree (every real component has a `CMakeLists.txt`), but worth knowing if
this guardrail is ever relied on to catch a bare/incomplete directory add.

**Guardrail semantics — the flip side of every other family's shrink-only
freeze.** A REMOVED component (e.g. the bb_pub_wifi deletion) prunes
normally via `--update-baseline`, same as any family. But a NEW component
not already in the baseline is exactly the case this fence exists to catch
— it FAILS, by design, until a human deliberately approves it.

**Rename detection (B1-1015).** Unlike `di_legacy` (keyed on SYMBOL, so a
pure file/path rename never changes identity), this family keys identity on
the directory NAME itself — a component rename (e.g. `bb_prov` ->
`bb_wifi_prov`, B1-808) genuinely changes that identity, so the generic
`_base.diff()` pairing (same identity, different path) never applies across
the old and new names; it shows up as an ordinary net-new + prune-candidate
pair, tripping the create-guardrail for what is actually an identity-stable
rename of an already-approved component. `rename_pairs()` below closes that
gap with a narrow, deliberately conservative heuristic: when a fence run
sees EXACTLY one new component and EXACTLY one removed component, treat
that 1:1 pair as a rename — drop both from the new/removed lists so the run
passes with zero baseline edit (no `--approve`, no `--update-baseline`
swap). Any other shape (zero removed, multiple new/removed) is genuinely
ambiguous and is left untouched, falling back to the normal net-new
guardrail — a real new component alongside an unrelated deletion in the
same run still requires `--approve` as long as either side has more than
one entry.

**Grow-by-approval — the one sanctioned additive baseline path.**
`--update-baseline` stays shrink-only for every family, this one included —
that invariant is never broken. Approving a new component instead goes
through a distinct, narrowly-scoped `fence --approve <component>` flag
(wired in `commands/fence_cmd.py`): it verifies `components/<component>/`
actually exists on disk, then appends exactly that one entry to this
family's baseline. Nothing else in the tree, and no other family, is
touched. The resulting one-line baseline diff on `new_component.json` *is*
the reviewed design sign-off — it shows up in the PR diff the same way any
other deliberate baseline edit would, but through a validated,
mistake-resistant command instead of hand-editing JSON.
"""
from __future__ import annotations
import os
import sys
from pathlib import Path
from typing import List, Set, Tuple

from discovery import build_index
from fence import _base
from fence._base import Marker

_COMPONENTS_DIR = "components"

# ---------------------------------------------------------------------------
# Marker-type -> reporting bucket
# ---------------------------------------------------------------------------

_BUCKETS = {
    "component": "new component",
}


def _bucket_for(marker_type: str) -> str:
    return _BUCKETS.get(marker_type, marker_type)


def counts_by_bucket(markers: Set[Marker]) -> dict:
    return _base.counts_by_bucket(markers, bucket_fn=_bucket_for)


# ---------------------------------------------------------------------------
# component — one marker per directory directly under components/.
# ---------------------------------------------------------------------------

def _scan_new_component(root: Path) -> Set[Marker]:
    """One marker per LEAF component directory under `components/` (the
    discovery SSOT's leaf rule — `discovery.build_index`), not a fixed
    directly-under-`components/` scan: a group/intermediate directory with
    no `CMakeLists.txt` of its own is never itself a component. Byte-
    identical marker set to the old direct-child scan on today's flat tree
    (every leaf dir is still exactly one level under `components/`)."""
    found: Set[Marker] = set()
    index = build_index([str(root)])
    root_p = Path(os.path.realpath(str(root)))
    for name in index.names():
        comp_dir = index.component_dir(name)
        if comp_dir is None:
            continue
        if comp_dir.name in _base.EXCLUDE_DIRS:
            continue
        path = comp_dir.relative_to(root_p).as_posix()
        found.add(Marker("component", path, name))
    return found


def scan_all(root: str) -> Set[Marker]:
    return _base.scan_all(sys.modules[__name__], root)


# ---------------------------------------------------------------------------
# Rename detection (B1-1015) — see module docstring for the full rationale.
# ---------------------------------------------------------------------------

def rename_pairs(
    new: List[Marker], removed: List[Marker]
) -> Tuple[List[Marker], List[Marker]]:
    """Given the (new, removed) lists from `_base.diff()`, drop an
    unambiguous 1:1 new/removed pair — treated as an identity-stable
    directory rename, not a net-new component + a prune candidate. Any
    other shape is returned unchanged (still net-new + prune-candidate)."""
    if len(new) == 1 and len(removed) == 1:
        return [], []
    return new, removed
