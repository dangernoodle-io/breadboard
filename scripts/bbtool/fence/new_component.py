"""new_component fence family — the "no unauthorized components" guardrail.

Unlike every other fence family (drain-only ratchets against a legacy or
hand-rolled surface), this one guards a CREATION event: a brand-new
`components/<name>/` directory. Per the component-creation policy
(CLAUDE.md "Component creation" + wiki design/Component-Taxonomy#when-to-
create-a-new-component, KB #402), extending an existing component is the
default; a genuinely new component requires a distinct dependency, a real
consumer, and reviewed design sign-off — never a speculative/ad-hoc add.

Marker type scanned: `component` — one marker per directory directly under
`components/` (each subdir is one component). Identity is the component
name alone (`id`), so a directory rename is NOT treated as a new component
so long as the name is unchanged — the tree layout uses one flat
`components/<name>/` level, never nested, so path and name always agree in
practice; identity is name-keyed per the same rename-stability convention
every other family uses (see `_base.diff`).

**Guardrail semantics — the flip side of every other family's shrink-only
freeze.** A REMOVED component (e.g. the bb_pub_wifi deletion) prunes
normally via `--update-baseline`, same as any family. But a NEW component
not already in the baseline is exactly the case this fence exists to catch
— it FAILS, by design, until a human deliberately approves it.

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
import sys
from pathlib import Path
from typing import Set

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
    found: Set[Marker] = set()
    components_dir = root / _COMPONENTS_DIR
    if not components_dir.is_dir():
        return found
    for entry in components_dir.iterdir():
        if not entry.is_dir():
            continue
        if entry.name in _base.EXCLUDE_DIRS:
            continue
        name = entry.name
        found.add(Marker("component", f"{_COMPONENTS_DIR}/{name}", name))
    return found


def scan_all(root: str) -> Set[Marker]:
    return _base.scan_all(sys.modules[__name__], root)
