"""wire_graph tests: per-tier topo-sort over synthetic InitEntry lists
(decision #735)."""
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from wire_graph import CycleError, MissingProviderError, topo_sort
from wire_parse import InitEntry


def entry(tier, fn, order=None, server=False, provides=(), requires=(),
          consumes=None, src_file="fake.h", src_line=1):
    return InitEntry(
        tier=tier, fn=fn, order=order, server=server,
        provides=tuple(provides), requires=tuple(requires),
        consumes=consumes, src_file=src_file, src_line=src_line,
    )


class TestLinearOrder(unittest.TestCase):
    def test_requires_provides_orders_dependency_first(self):
        entries = [
            entry("early", "bb_config_init", requires=["stream"]),
            entry("early", "bb_stream_init", provides=["stream"]),
        ]
        ordered = topo_sort(entries)
        self.assertEqual([e.fn for e in ordered], ["bb_stream_init", "bb_config_init"])

    def test_chain_of_three(self):
        entries = [
            entry("early", "c", requires=["b"], provides=["c"]),
            entry("early", "a", provides=["a"]),
            entry("early", "b", requires=["a"], provides=["b"]),
        ]
        ordered = topo_sort(entries)
        self.assertEqual([e.fn for e in ordered], ["a", "b", "c"])


class TestTierOrdering(unittest.TestCase):
    def test_early_before_pre_http_before_regular_regardless_of_input_order(self):
        entries = [
            entry("regular", "r"),
            entry("pre_http", "p"),
            entry("early", "e"),
        ]
        ordered = topo_sort(entries)
        self.assertEqual([e.fn for e in ordered], ["e", "p", "r"])

    def test_requires_satisfied_by_earlier_tier_needs_no_same_tier_provider(self):
        entries = [
            entry("early", "e", provides=["core"]),
            entry("regular", "r", requires=["core"]),
        ]
        ordered = topo_sort(entries)
        self.assertEqual([e.fn for e in ordered], ["e", "r"])

    def test_requires_from_later_tier_is_missing_provider_error(self):
        entries = [
            entry("early", "e", requires=["core"]),
            entry("regular", "r", provides=["core"]),
        ]
        with self.assertRaises(MissingProviderError):
            topo_sort(entries)


class TestTieBreak(unittest.TestCase):
    def test_explicit_order_ascending_wins_over_parse_order(self):
        entries = [
            entry("early", "second", order=2),
            entry("early", "first", order=1),
        ]
        ordered = topo_sort(entries)
        self.assertEqual([e.fn for e in ordered], ["first", "second"])

    def test_no_order_falls_back_to_parse_order(self):
        entries = [
            entry("early", "a"),
            entry("early", "b"),
            entry("early", "c"),
        ]
        ordered = topo_sort(entries)
        self.assertEqual([e.fn for e in ordered], ["a", "b", "c"])

    def test_entries_without_order_sort_after_entries_with_order(self):
        entries = [
            entry("early", "no_order"),
            entry("early", "has_order", order=99),
        ]
        ordered = topo_sort(entries)
        self.assertEqual([e.fn for e in ordered], ["has_order", "no_order"])


class TestConsumesNoSpecialOrdering(unittest.TestCase):
    def test_consumes_entry_sorts_by_order_and_parse_order_like_any_entry(self):
        """A `consumes=` entry carries no requires/provides edge — it sorts
        purely by (order, parse-order), same as an ordinary entry. Locks the
        design decision that setter-injection adds no new ordering/tier
        mechanism."""
        entries = [
            entry("early", "second", order=2, consumes="demo_sink"),
            entry("early", "first", order=1),
        ]
        ordered = topo_sort(entries)
        self.assertEqual([e.fn for e in ordered], ["first", "second"])


class TestConsumesOrderBeforeAutoinit(unittest.TestCase):
    def test_ordered_setter_sorts_before_unordered_autoinit_same_tier(self):
        """Regression (B1-741 review HIGH): a `consumes=`-shaped setter entry
        with an explicit `order=` must sort BEFORE a plain autoinit-shaped
        entry with no `order=` in the same tier, even when the autoinit entry
        appears first in parse order -- mirrors bb_wifi_set_emit(order=0)
        vs. bb_wifi_autoinit(no order) in the real wifi.net emit-seam wire:
        the setter must register the emit sink before autoinit can fire the
        first boot-window wifi.net edge into it, or that edge is silently
        dropped (bb_wifi_emit_baseline only re-synthesizes CURRENT state
        later, it doesn't replay missed transient edges)."""
        entries = [
            entry("early", "bb_wifi_autoinit"),
            entry("early", "bb_wifi_set_emit", order=0, consumes="emit_sink"),
        ]
        ordered = topo_sort(entries)
        self.assertEqual(
            [e.fn for e in ordered], ["bb_wifi_set_emit", "bb_wifi_autoinit"])


class TestCycle(unittest.TestCase):
    def test_direct_cycle_raises(self):
        entries = [
            entry("early", "a", provides=["a"], requires=["b"]),
            entry("early", "b", provides=["b"], requires=["a"]),
        ]
        with self.assertRaises(CycleError):
            topo_sort(entries)


class TestMissingProvider(unittest.TestCase):
    def test_requires_nothing_provides_anywhere_raises(self):
        entries = [entry("early", "a", requires=["ghost"])]
        with self.assertRaises(MissingProviderError):
            topo_sort(entries)


if __name__ == "__main__":
    unittest.main()
