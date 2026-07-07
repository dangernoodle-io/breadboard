"""AUTOWIRE topo-sort (decision #735): order a flat list of `InitEntry`
(wire_parse.InitEntry) into the sequence `bb_app_init` calls them in.

Outer key = tier, fixed order early < pre_http < regular (wire_parse.TIERS) —
tiers are never interleaved; a `regular` entry never runs before a
`pre_http` entry, even if requires/provides would otherwise allow it.

Inner key, within one tier: Kahn's algorithm over requires -> provides edges
(an entry requiring key `k` runs after whichever entry in the SAME tier
provides `k`). A `requires` key satisfied by an EARLIER tier's `provides` is
fine (tier ordering already guarantees it) and adds no edge. A `requires` key
provided only by a LATER tier, or not provided anywhere, is a hard
MissingProviderError.

Tie-break among entries with no ordering edge between them: explicit
`order=` ascending (entries without `order` sort after all entries that have
one), then parse order (the entries list's own input order) — this
reproduces bb_init's old `.order` stable-sort.
"""
from __future__ import annotations
from typing import Dict, List, Set

from wire_parse import TIER_RANK, InitEntry


class CycleError(Exception):
    """Raised when a tier's requires/provides graph has a cycle."""


class MissingProviderError(Exception):
    """Raised when an entry `requires` a key nothing provides in the same or
    an earlier tier."""


def _tie_key(entry: InitEntry, idx: int):
    order = entry.order if entry.order is not None else float("inf")
    return (order, idx)


def topo_sort(entries: List[InitEntry]) -> List[InitEntry]:
    """Return `entries` ordered: tier-grouped (early, pre_http, regular),
    each tier internally topo-sorted by requires->provides with
    (order, parse-order) tie-break. Raises CycleError / MissingProviderError."""
    # Global provides map (key -> earliest tier rank that provides it), used
    # to validate cross-tier requires satisfaction.
    provides_tier: Dict[str, int] = {}
    for entry in entries:
        rank = TIER_RANK[entry.tier]
        for key in entry.provides:
            if key not in provides_tier or rank < provides_tier[key]:
                provides_tier[key] = rank

    result: List[InitEntry] = []
    for tier in sorted(TIER_RANK, key=TIER_RANK.get):
        tier_rank = TIER_RANK[tier]
        # (entry, original parse-index) pairs for this tier, preserving
        # overall parse order for tie-breaking.
        tier_entries = [(idx, e) for idx, e in enumerate(entries) if e.tier == tier]

        # provides -> entry positions (within this tier)
        provider_of: Dict[str, List[int]] = {}
        for pos, (_, e) in enumerate(tier_entries):
            for key in e.provides:
                provider_of.setdefault(key, []).append(pos)

        n = len(tier_entries)
        # Build edges: pos depends on provider positions for same-tier
        # requires; validate cross-tier requires.
        deps: List[Set[int]] = [set() for _ in range(n)]
        dependents: List[Set[int]] = [set() for _ in range(n)]
        for pos, (_, e) in enumerate(tier_entries):
            for key in e.requires:
                if key in provider_of:
                    for provider_pos in provider_of[key]:
                        if provider_pos != pos:
                            deps[pos].add(provider_pos)
                            dependents[provider_pos].add(pos)
                    continue
                provided_rank = provides_tier.get(key)
                if provided_rank is None or provided_rank > tier_rank:
                    raise MissingProviderError(
                        f"{e.src_file}:{e.src_line}: fn={e.fn} requires "
                        f"'{key}', which nothing provides in tier "
                        f"'{tier}' or an earlier tier"
                    )
                # provided by a strictly earlier tier: satisfied by tier
                # ordering alone, no edge needed.

        in_degree = [len(d) for d in deps]
        ready = [pos for pos in range(n) if in_degree[pos] == 0]

        ordered_positions: List[int] = []
        while ready:
            ready.sort(key=lambda pos: _tie_key(tier_entries[pos][1], tier_entries[pos][0]))
            pos = ready.pop(0)
            ordered_positions.append(pos)
            for dependent in sorted(dependents[pos]):
                in_degree[dependent] -= 1
                if in_degree[dependent] == 0:
                    ready.append(dependent)

        if len(ordered_positions) != n:
            stuck = [tier_entries[pos][1] for pos in range(n) if pos not in ordered_positions]
            names = ", ".join(f"{e.fn} ({e.src_file}:{e.src_line})" for e in stuck)
            raise CycleError(f"tier '{tier}' has a requires/provides cycle among: {names}")

        result.extend(tier_entries[pos][1] for pos in ordered_positions)

    return result
