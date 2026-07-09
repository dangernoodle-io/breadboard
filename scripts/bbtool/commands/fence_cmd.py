"""fence command — unified ratchet-fence lint over one or more families.

A "family" (see `scripts/bbtool/fence/`) is a group of marker scanners plus
its own committed baseline at `.baseline/bbtool/fence/<family>.json`. This
command scans the requested families (default: all discovered), diffs each
against its baseline, and FAILS if any family has a net-new occurrence not
already in its baseline. Removals are never a failure — they are reported
as INFO "candidate to prune" so a baseline can shrink over time.

`--update-baseline` is SHRINK-ONLY: it prunes baseline entries whose
occurrence no longer exists, but never adds a net-new occurrence to the
baseline — a fresh duplicate/new marker always stays a failure until it is
either removed from the tree or the baseline is knowingly hand-edited. Use
`--seed <family>` exactly once, for a brand-new family with no baseline
yet, to bless its current occurrence set wholesale as the starting point.

`--approve <component>` is a narrow, sanctioned exception to the
shrink-only rule above, scoped to the `new_component` family only (see
`fence/new_component.py`): it appends exactly one already-on-disk
`components/<component>/` to that family's baseline, and never touches any
other family's baseline. `--update-baseline` itself stays shrink-only for
every family, `new_component` included.

The `di-fence` command (`scripts/bbtool/commands/di_fence.py`) is a thin
back-compat alias for `fence --family di_legacy`.
"""
from __future__ import annotations
import argparse
import os
import sys

import fence as fence_pkg

NAME = "fence"
HELP = "Ratchet-fence lint over one or more marker families (see fence/ package)"


def add_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument(
        "--root",
        default=None,
        help="repository root (default: from top-level --root or cwd)",
    )
    parser.add_argument(
        "--family",
        action="append",
        default=None,
        metavar="NAME",
        help="restrict to this family (repeatable); default: all discovered families",
    )
    parser.add_argument(
        "--update-baseline",
        action="store_true",
        help="shrink-only: prune baseline entries no longer found; never blesses"
             " a net-new occurrence (use --seed for a brand-new family)",
    )
    parser.add_argument(
        "--seed",
        default=None,
        metavar="FAMILY",
        help="one-time: bless the current occurrence set of FAMILY as its"
             " starting baseline (errors if a baseline already exists)",
    )
    parser.add_argument(
        "--approve",
        default=None,
        metavar="COMPONENT",
        help="new_component family only: append the single named component"
             " (must already exist at components/COMPONENT/) to the"
             " new_component baseline — the sanctioned grow-by-approval"
             " path; does not touch any other family's baseline",
    )


def _resolve_root(args: argparse.Namespace) -> str:
    root = getattr(args, "root", None) or getattr(args, "_root_abs", None) or os.getcwd()
    return os.path.abspath(root)


def _resolve_families(args: argparse.Namespace):
    names = getattr(args, "family", None) or sorted(fence_pkg.FAMILIES)
    unknown = [n for n in names if n not in fence_pkg.FAMILIES]
    if unknown:
        print(
            f"bbtool fence: unknown family/families: {', '.join(unknown)}"
            f" (known: {', '.join(sorted(fence_pkg.FAMILIES))})",
            file=sys.stderr,
        )
        return None
    return names


def _seed(root: str, family: str) -> int:
    if family not in fence_pkg.FAMILIES:
        print(
            f"bbtool fence: unknown family '{family}'"
            f" (known: {', '.join(sorted(fence_pkg.FAMILIES))})",
            file=sys.stderr,
        )
        return 1
    path = fence_pkg.baseline_path(root, family)
    if path.is_file():
        print(
            f"bbtool fence[{family}]: baseline already exists at {path}"
            " — use --update-baseline to prune it, --seed is one-time only",
            file=sys.stderr,
        )
        return 1
    module = fence_pkg.FAMILIES[family]
    current = fence_pkg.scan_all(module, root)
    written = fence_pkg.write_baseline(root, family, current)
    print(f"bbtool fence[{family}]: baseline seeded ({len(current)} entries) -> {written}")
    return 0


_APPROVE_FAMILY = "new_component"


def _approve(root: str, component: str) -> int:
    """Grow-by-approval: append exactly one component to the
    new_component family's baseline. This is the ONE sanctioned additive
    baseline path in the whole fence system — every family (this one
    included) stays shrink-only under `--update-baseline`; approving a new
    component is a distinct, narrowly-scoped operation that never touches
    any other family."""
    if _APPROVE_FAMILY not in fence_pkg.FAMILIES:
        print(
            f"bbtool fence: --approve requires the '{_APPROVE_FAMILY}' family,"
            " which is not present in this tree",
            file=sys.stderr,
        )
        return 1
    component_dir = os.path.join(root, "components", component)
    if not os.path.isdir(component_dir):
        print(
            f"bbtool fence --approve: components/{component} does not exist"
            " on disk — --approve only blesses a component that is already"
            " present in the tree, it never creates one",
            file=sys.stderr,
        )
        return 1
    module = fence_pkg.FAMILIES[_APPROVE_FAMILY]
    identity_fn = fence_pkg.identity_fn_for(module)
    current = fence_pkg.scan_all(module, root)
    marker = next(
        (m for m in current if m.type == "component" and m.id == component),
        None,
    )
    if marker is None:
        print(
            f"bbtool fence --approve: components/{component} is not a"
            " scanned component directory (unexpected) — nothing to approve",
            file=sys.stderr,
        )
        return 1
    baseline = fence_pkg.load_baseline(root, _APPROVE_FAMILY)
    baseline_ids = {identity_fn(m) for m in baseline}
    if identity_fn(marker) in baseline_ids:
        print(
            f"bbtool fence[{_APPROVE_FAMILY}]: components/{component} is"
            " already approved (already in the baseline) — nothing to do"
        )
        return 0
    updated = baseline | {marker}
    written = fence_pkg.write_baseline(root, _APPROVE_FAMILY, updated)
    print(
        f"bbtool fence[{_APPROVE_FAMILY}]: approved components/{component}"
        f" -> {written}"
    )
    return 0


def _update_baseline(root: str, family: str) -> int:
    module = fence_pkg.FAMILIES[family]
    identity_fn = fence_pkg.identity_fn_for(module)
    current = fence_pkg.scan_all(module, root)
    baseline = fence_pkg.load_baseline(root, family)
    if not baseline and not fence_pkg.baseline_path(root, family).is_file():
        print(
            f"bbtool fence[{family}]: no baseline yet — use --seed {family} first",
            file=sys.stderr,
        )
        return 1
    new, removed = fence_pkg.diff(current, baseline, identity_fn)
    removed_ids = {identity_fn(m) for m in removed}
    surviving = {m for m in baseline if identity_fn(m) not in removed_ids}
    written = fence_pkg.write_baseline(root, family, surviving)
    print(
        f"bbtool fence[{family}]: baseline pruned ({len(removed)} removed,"
        f" {len(surviving)} kept) -> {written}"
    )
    if new:
        print(
            f"INFO [fence:{family}]: {len(new)} new marker(s) NOT added to the"
            " baseline (--update-baseline is shrink-only) — still failing on a"
            " normal run"
        )
    return 0


def _check(root: str, family: str) -> bool:
    module = fence_pkg.FAMILIES[family]
    identity_fn = fence_pkg.identity_fn_for(module)
    current = fence_pkg.scan_all(module, root)
    baseline = fence_pkg.load_baseline(root, family)
    new, removed = fence_pkg.diff(current, baseline, identity_fn)

    for m in removed:
        print(f"INFO [fence:{family}]: candidate to prune from baseline: {m.path}:{m.id} ({m.type})")

    if new:
        for m in new:
            print(
                f"ERROR [fence:{family}]: new marker added: {m.path}:{m.id} ({m.type})"
                f" — the {family} surface is frozen shrink-only; compose instead,"
                " or if this is a legitimate baseline change, seed/edit the"
                " baseline deliberately",
                file=sys.stderr,
            )
        print(f"bbtool fence[{family}]: {len(new)} new marker(s) — FAIL", file=sys.stderr)
        return False

    print(f"bbtool fence[{family}]: {len(current)} marker(s), 0 new — PASS")
    return True


def run(args: argparse.Namespace) -> int:
    root = _resolve_root(args)

    if getattr(args, "seed", None) and getattr(args, "update_baseline", False):
        print("bbtool fence: --seed and --update-baseline are mutually exclusive", file=sys.stderr)
        return 1

    if getattr(args, "seed", None) and getattr(args, "family", None):
        print(
            "bbtool fence: --seed and --family are mutually exclusive"
            " — --seed already names the single target family",
            file=sys.stderr,
        )
        return 1

    if getattr(args, "approve", None) is not None and (
        getattr(args, "seed", None)
        or getattr(args, "update_baseline", False)
        or getattr(args, "family", None)
    ):
        print(
            "bbtool fence: --approve is mutually exclusive with --seed,"
            " --update-baseline, and --family — it already names the"
            " single component being approved into new_component",
            file=sys.stderr,
        )
        return 1

    if getattr(args, "approve", None) is not None:
        return _approve(root, args.approve)

    if getattr(args, "seed", None):
        return _seed(root, args.seed)

    families = _resolve_families(args)
    if families is None:
        return 1

    if getattr(args, "update_baseline", False):
        for family in families:
            rc = _update_baseline(root, family)
            if rc != 0:
                return rc
        return 0

    all_pass = True
    for family in families:
        if not _check(root, family):
            all_pass = False
    return 0 if all_pass else 1


def register(api) -> None:
    api.add_command(NAME, sys.modules[__name__])
