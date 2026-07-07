"""wire_graph tests: per-tier topo-sort over synthetic InitEntry lists
(decision #735)."""
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from wire_graph import CycleError, MissingProviderError, topo_sort
from wire_parse import InitEntry


def entry(tier, fn, order=None, server=False, provides=(), requires=(),
          src_file="fake.h", src_line=1):
    return InitEntry(
        tier=tier, fn=fn, order=order, server=server,
        provides=tuple(provides), requires=tuple(requires),
        src_file=src_file, src_line=src_line,
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
