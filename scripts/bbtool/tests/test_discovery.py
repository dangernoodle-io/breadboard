"""discovery.py tests (B1-979): single-root discovery, cross-tree collision
detection, index lookups, and owner_of_path — over synthetic temp-dir
fixtures (mirrors test_boards.py's fixture style), plus a real-tree sanity
check."""
import os
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from discovery import CollisionError, build_index


class _DiscoveryTestCase(unittest.TestCase):
    """Base class clearing the memoized-index cache (discovery.py's
    `build_index` is `lru_cache`-backed) before every test. Defensive:
    distinct temp roots already give distinct cache keys, but this keeps
    the suite hermetic against a stale entry leaking across tests."""

    def setUp(self) -> None:
        build_index.cache_clear()


def _write(path: Path, content: str = "") -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")


def _make_component(root: Path, name: str) -> None:
    _write(root / "components" / name / "CMakeLists.txt", "idf_component_register()\n")
    _write(root / "components" / name / "include" / f"{name}.h", "#pragma once\n")


def _make_platform(root: Path, layer: str, name: str, files=None) -> None:
    base = root / "platform" / layer / name
    for f in (files or ["x.c"]):
        _write(base / f, "// impl\n")


class TestSingleRootDiscovery(_DiscoveryTestCase):
    def test_component_only(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            _make_component(root, "bb_core")
            index = build_index([str(root)])
            self.assertEqual(index.names(), {"bb_core"})
            self.assertEqual(index.component_dir("bb_core"), root / "components" / "bb_core")
            self.assertEqual(index.platform_dir("bb_core", "host"), None)

    def test_platform_only(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            _make_platform(root, "espidf", "bb_routes_espidf")
            index = build_index([str(root)])
            self.assertEqual(index.names(), {"bb_routes_espidf"})
            self.assertIsNone(index.component_dir("bb_routes_espidf"))
            self.assertEqual(
                index.platform_dir("bb_routes_espidf", "espidf"),
                root / "platform" / "espidf" / "bb_routes_espidf",
            )

    def test_component_and_platform(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            _make_component(root, "bb_foo")
            _make_platform(root, "host", "bb_foo")
            _make_platform(root, "espidf", "bb_foo")
            index = build_index([str(root)])
            self.assertEqual(index.names(), {"bb_foo"})
            self.assertIsNotNone(index.component_dir("bb_foo"))
            self.assertIsNotNone(index.platform_dir("bb_foo", "host"))
            self.assertIsNotNone(index.platform_dir("bb_foo", "espidf"))
            self.assertIsNone(index.platform_dir("bb_foo", "arduino"))

    def test_empty_tree(self):
        with tempfile.TemporaryDirectory() as tmp:
            index = build_index([tmp])
            self.assertEqual(index.names(), set())


class TestCollisionDetection(_DiscoveryTestCase):
    def test_same_name_two_component_roots_collides(self):
        with tempfile.TemporaryDirectory() as tmp_a, tempfile.TemporaryDirectory() as tmp_b:
            root_a, root_b = Path(tmp_a), Path(tmp_b)
            _make_component(root_a, "bb_dup")
            _make_component(root_b, "bb_dup")
            with self.assertRaises(CollisionError) as cm:
                build_index([str(root_a), str(root_b)])
            self.assertIn("bb_dup", str(cm.exception))

    def test_components_vs_platform_layer_collides(self):
        with tempfile.TemporaryDirectory() as tmp_a, tempfile.TemporaryDirectory() as tmp_b:
            root_a, root_b = Path(tmp_a), Path(tmp_b)
            _make_component(root_a, "bb_dup")
            _make_platform(root_b, "espidf", "bb_dup")
            with self.assertRaises(CollisionError):
                build_index([str(root_a), str(root_b)])

    def test_disjoint_names_union_without_error(self):
        with tempfile.TemporaryDirectory() as tmp_a, tempfile.TemporaryDirectory() as tmp_b:
            root_a, root_b = Path(tmp_a), Path(tmp_b)
            _make_component(root_a, "bb_a")
            _make_component(root_b, "bb_b")
            index = build_index([str(root_a), str(root_b)])
            self.assertEqual(index.names(), {"bb_a", "bb_b"})


class TestIndexLookups(_DiscoveryTestCase):
    def test_unknown_name_returns_none(self):
        with tempfile.TemporaryDirectory() as tmp:
            index = build_index([tmp])
            self.assertIsNone(index.entry("ghost"))
            self.assertIsNone(index.component_dir("ghost"))
            self.assertIsNone(index.platform_dir("ghost", "host"))

    def test_entry_fields(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            _make_component(root, "bb_core")
            _make_platform(root, "host", "bb_core")
            index = build_index([str(root)])
            entry = index.entry("bb_core")
            self.assertEqual(entry.name, "bb_core")
            self.assertEqual(entry.root, str(root))
            self.assertEqual(entry.component_dir, root / "components" / "bb_core")
            self.assertEqual(entry.platform_dirs, {"host": root / "platform" / "host" / "bb_core"})


class TestOwnerOfPath(_DiscoveryTestCase):
    def test_owner_under_components(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            _make_component(root, "bb_foo")
            src = root / "components" / "bb_foo" / "src" / "x.c"
            _write(src, "// x\n")
            index = build_index([str(root)])
            self.assertEqual(index.owner_of_path(src), "bb_foo")

    def test_owner_under_platform(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            _make_platform(root, "espidf", "bb_foo", files=["x.c"])
            path = root / "platform" / "espidf" / "bb_foo" / "x.c"
            index = build_index([str(root)])
            self.assertEqual(index.owner_of_path(path), "bb_foo")

    def test_relative_path_also_resolves(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            _make_component(root, "bb_foo")
            index = build_index([str(root)])
            rel = Path("components/bb_foo/src/x.c")
            self.assertEqual(index.owner_of_path(rel), "bb_foo")

    def test_outside_convention_returns_none(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            _make_component(root, "bb_foo")
            index = build_index([str(root)])
            self.assertIsNone(index.owner_of_path(root / "README.md"))
            self.assertIsNone(index.owner_of_path(root / "components" / "README.md"))

    def test_unknown_owner_name_returns_none(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            _make_component(root, "bb_foo")
            index = build_index([str(root)])
            path = root / "components" / "bb_ghost" / "src" / "x.c"
            self.assertIsNone(index.owner_of_path(path))


class TestRootsNormalization(_DiscoveryTestCase):
    """#6/#7: `build_index` dedups `roots` at entry and tolerates an empty
    `roots` list."""

    def test_empty_roots_returns_empty_index(self):
        index = build_index([])
        self.assertEqual(index.names(), set())

    def test_duplicate_root_does_not_self_collide(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            _make_component(root, "bb_dup_root")
            single = build_index([str(root)])
            duped = build_index([str(root), str(root)])
            self.assertEqual(duped.names(), single.names())
            self.assertEqual(duped.names(), {"bb_dup_root"})


class TestRealTreeSanity(_DiscoveryTestCase):
    """B1-979: the real repo tree, single root, resolves without error and
    every discovered name owns its own directory's paths."""

    ROOT = str(Path(__file__).resolve().parents[3])

    def test_real_tree_discovers_and_owns_paths(self):
        index = build_index([self.ROOT])
        names = index.names()
        self.assertGreater(len(names), 5)
        sample = sorted(names)[0]
        entry = index.entry(sample)
        self.assertEqual(entry.name, sample)
        if entry.component_dir is not None:
            probe = entry.component_dir / "CMakeLists.txt"
            self.assertEqual(index.owner_of_path(probe), sample)


if __name__ == "__main__":
    unittest.main()
