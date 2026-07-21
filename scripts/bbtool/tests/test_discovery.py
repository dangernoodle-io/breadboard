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
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "commands"))

from discovery import CollisionError, build_index, normalize_roots
from boards import derive_component
from composition import resolve_composition
from commands.wire import collect_entries, collect_provides_entries


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
            # realpath'd up front: `build_index` canonicalizes internally, so
            # comparing its output against a non-canonical `root` Path would
            # spuriously fail on a platform where the OS temp dir itself
            # sits behind a symlink (e.g. macOS /var -> /private/var).
            root = Path(os.path.realpath(tmp))
            _make_component(root, "bb_core")
            index = build_index([str(root)])
            self.assertEqual(index.names(), {"bb_core"})
            self.assertEqual(index.component_dir("bb_core"), root / "components" / "bb_core")
            self.assertEqual(index.platform_dir("bb_core", "host"), None)

    def test_platform_only(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(os.path.realpath(tmp))
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
            root = Path(os.path.realpath(tmp))
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
            # See test_component_only's comment: realpath up front so the
            # expected `entry.root`/`entry.component_dir` match
            # `build_index`'s internal canonicalization.
            root = Path(os.path.realpath(tmp))
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
            # realpath up front — `owner_of_path` matches `path` against
            # this index's (now canonicalized) roots via `relative_to`, so a
            # non-canonical `src` built from a symlink-alias `root` would
            # never match and would spuriously return `None`.
            root = Path(os.path.realpath(tmp))
            _make_component(root, "bb_foo")
            src = root / "components" / "bb_foo" / "src" / "x.c"
            _write(src, "// x\n")
            index = build_index([str(root)])
            self.assertEqual(index.owner_of_path(src), "bb_foo")

    def test_owner_under_platform(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(os.path.realpath(tmp))
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


def _make_component_with_src(root: Path, name: str) -> None:
    comp = root / "components" / name
    _write(comp / "CMakeLists.txt", "idf_component_register()\n")
    _write(comp / "include" / f"{name}.h", "#pragma once\n")
    _write(comp / "src" / f"{name}.c", "// src\n")


def _make_component_with_marker(root: Path, name: str, marker: str) -> None:
    comp = root / "components" / name
    _write(comp / "CMakeLists.txt", "idf_component_register()\n")
    _write(comp / "include" / f"{name}.h", f"#pragma once\n{marker}void {name}_init(void);\n")


class TestMultiRootOwningRoot(_DiscoveryTestCase):
    """B1-1084: `boards.derive_component`, `composition.resolve_composition`,
    and `commands.wire.collect_entries`/`collect_provides_entries` must
    resolve each component against its OWN owning root (`ComponentEntry.root`
    from the discovery index built over every root), never blindly against
    `roots[0]` -- the load-bearing fix this ticket is about. `roots[0]` here
    (`root_a`) never contains the component under test, so a stale
    single-root resolution would either raise (paths not relative to
    `root_a`) or silently miss the component entirely -- these tests would
    fail loudly under the pre-fix behavior."""

    def test_derive_component_resolves_under_owning_root_not_primary(self):
        with tempfile.TemporaryDirectory() as tmp_a, tempfile.TemporaryDirectory() as tmp_b:
            root_a, root_b = Path(tmp_a), Path(tmp_b)
            _make_component(root_a, "bb_only_a")
            _make_component_with_src(root_b, "bb_only_b")
            entry = derive_component([str(root_a), str(root_b)], "bb_only_b", "host")
            self.assertEqual(
                entry["includes"],
                ["components/bb_only_b/include", "components/bb_only_b/src"],
            )
            self.assertEqual(entry["sources"], ["components/bb_only_b/src/bb_only_b.c"])

    def test_resolve_composition_collision_names_both_roots(self):
        with tempfile.TemporaryDirectory() as tmp_a, tempfile.TemporaryDirectory() as tmp_b:
            root_a, root_b = Path(tmp_a), Path(tmp_b)
            _make_component(root_a, "bb_dup")
            _make_component(root_b, "bb_dup")
            with self.assertRaises(CollisionError) as cm:
                resolve_composition([str(root_a), str(root_b)], ["bb_dup"], platform="host")
            msg = str(cm.exception)
            self.assertIn(str(root_a), msg)
            self.assertIn(str(root_b), msg)

    def test_collect_entries_grep_and_src_file_for_root_b_component(self):
        with tempfile.TemporaryDirectory() as tmp_a, tempfile.TemporaryDirectory() as tmp_b:
            root_a, root_b = Path(tmp_a), Path(tmp_b)
            _make_component(root_a, "bb_primary_only")
            _make_component_with_marker(
                root_b, "bb_extra", "// bbtool:init tier=early fn=bb_extra_init\n",
            )
            roots = [str(root_a), str(root_b)]

            entries = collect_entries(roots, ["bb_extra"], "host")
            self.assertEqual([e.fn for e in entries], ["bb_extra_init"])
            # Fork 2: a component under a NON-primary root gets an absolute
            # src_file -- never a bare "components/bb_extra/..." relative
            # path that could visually collide with a same-shaped path under
            # root_a.
            self.assertTrue(os.path.isabs(entries[0].src_file))
            # `collect_entries` normalizes `roots` (realpath, Finding 1) before
            # resolving each component's owning root, so the recorded
            # `src_file` is prefixed with root_b's CANONICAL form -- which may
            # differ in spelling from `str(root_b)` on a platform where the
            # temp dir itself sits behind a symlink (e.g. macOS /var ->
            # /private/var).
            self.assertTrue(entries[0].src_file.startswith(os.path.realpath(str(root_b))))

            provides = collect_provides_entries(roots, ["bb_extra"], "host")
            self.assertEqual(provides, [])

    def test_primary_root_component_keeps_relative_src_file(self):
        """Back-compat companion to the above: a component under `roots[0]`
        (the primary root) keeps the plain repo-root-relative `src_file` --
        byte-identical to pre-B1-1084 single-root output -- even in a
        multi-root call, since `entry_root == primary_root` for it."""
        with tempfile.TemporaryDirectory() as tmp_a, tempfile.TemporaryDirectory() as tmp_b:
            root_a, root_b = Path(tmp_a), Path(tmp_b)
            _make_component_with_marker(
                root_a, "bb_primary", "// bbtool:init tier=early fn=bb_primary_init\n",
            )
            _make_component(root_b, "bb_only_b")
            entries = collect_entries([str(root_a), str(root_b)], ["bb_primary"], "host")
            self.assertEqual(
                entries[0].src_file, "components/bb_primary/include/bb_primary.h"
            )


class TestNormalizeRootsDirect(_DiscoveryTestCase):
    """#6: direct unit coverage of `normalize_roots` itself — the bare
    `os.PathLike` branch (a single `Path`, not wrapped in a list) is never
    hit by any other caller, since every real call site already passes
    either a bare `str` or a list."""

    def test_bare_str_root(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.assertEqual(normalize_roots(tmp), [os.path.realpath(tmp)])

    def test_bare_path_root(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.assertEqual(normalize_roots(Path(tmp)), [os.path.realpath(tmp)])

    def test_list_of_mixed_path_and_str_dedupes(self):
        with tempfile.TemporaryDirectory() as tmp_a, tempfile.TemporaryDirectory() as tmp_b:
            result = normalize_roots([Path(tmp_a), str(tmp_a), tmp_b])
            self.assertEqual(result, [os.path.realpath(tmp_a), os.path.realpath(tmp_b)])


class TestNormalizeRootsCanonicalization(_DiscoveryTestCase):
    """FINDING 1 (HIGH): two spellings of the SAME physical directory must
    canonicalize to one root before dedup/collision-check — a symlink alias,
    a trailing slash, or a relative `./`/`../` form must never raise a
    spurious `CollisionError` just because the string spelling differs.
    A genuine collision (two DISTINCT directories, same component name)
    must still raise."""

    def test_symlink_alias_of_same_dir_collapses_no_collision(self):
        with tempfile.TemporaryDirectory() as tmp:
            real = Path(tmp) / "real"
            _make_component(real, "bb_sym")
            alias = Path(tmp) / "alias"
            alias.symlink_to(real, target_is_directory=True)
            index = build_index(normalize_roots([str(real), str(alias)]))
            self.assertEqual(index.names(), {"bb_sym"})

    def test_trailing_slash_spelling_collapses_no_collision(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            _make_component(root, "bb_slash")
            index = build_index(normalize_roots([str(root), str(root) + os.sep]))
            self.assertEqual(index.names(), {"bb_slash"})

    def test_relative_form_spelling_collapses_no_collision(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            _make_component(root, "bb_rel")
            rel_form = os.path.join(str(root), ".", "..", root.name)
            index = build_index(normalize_roots([str(root), rel_form]))
            self.assertEqual(index.names(), {"bb_rel"})

    def test_genuine_distinct_dir_collision_still_raises(self):
        with tempfile.TemporaryDirectory() as tmp_a, tempfile.TemporaryDirectory() as tmp_b:
            root_a, root_b = Path(tmp_a), Path(tmp_b)
            _make_component(root_a, "bb_real_dup")
            _make_component(root_b, "bb_real_dup")
            with self.assertRaises(CollisionError):
                build_index(normalize_roots([str(root_a), str(root_b)]))


class TestBuildIndexCanonicalizesDirectly(_DiscoveryTestCase):
    """Structural nit (review, B1-979 follow-up): `build_index()` must
    canonicalize by construction, not merely by convention through
    `normalize_roots`. Proof the pre-fix version was bypassable:
    `commands/lint.py`'s `_emit_seam_owner_from_path` calls
    `build_index([str(root)])` directly, skipping `normalize_roots`
    entirely — harmless with today's single-root call, but the next such
    direct caller with a symlink-aliased or trailing-slash-spelled root
    would reintroduce the original HIGH. These tests call `build_index`
    ALONE, with raw un-normalized spellings, mirroring that bypass path."""

    def test_symlink_alias_dedupes_without_normalize_roots(self):
        with tempfile.TemporaryDirectory() as tmp:
            real = Path(tmp) / "real"
            _make_component(real, "bb_direct_sym")
            alias = Path(tmp) / "alias"
            alias.symlink_to(real, target_is_directory=True)
            # No normalize_roots() call — raw string spellings straight into
            # build_index, exactly like lint.py's bypass path.
            index = build_index([str(real), str(alias)])
            self.assertEqual(index.names(), {"bb_direct_sym"})

    def test_trailing_slash_dedupes_without_normalize_roots(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            _make_component(root, "bb_direct_slash")
            index = build_index([str(root), str(root) + os.sep])
            self.assertEqual(index.names(), {"bb_direct_slash"})

    def test_genuine_distinct_dir_collision_still_raises_without_normalize_roots(self):
        with tempfile.TemporaryDirectory() as tmp_a, tempfile.TemporaryDirectory() as tmp_b:
            root_a, root_b = Path(tmp_a), Path(tmp_b)
            _make_component(root_a, "bb_direct_real_dup")
            _make_component(root_b, "bb_direct_real_dup")
            with self.assertRaises(CollisionError):
                build_index([str(root_a), str(root_b)])


def _make_nested_component(root: Path, *group_path, name: str) -> None:
    """Write `components/<group_path...>/<name>/CMakeLists.txt` (+ a header
    under `include/`), mirroring `_make_component`'s flat-layout shape one
    or more group levels deeper."""
    comp = root.joinpath("components", *group_path, name)
    _write(comp / "CMakeLists.txt", "idf_component_register()\n")
    _write(comp / "include" / f"{name}.h", "#pragma once\n")


class TestLeafRuleDiscovery(_DiscoveryTestCase):
    """PR1 of the depth-agnostic-layout lane: component identity is the
    innermost directory containing a `CMakeLists.txt`, not a fixed depth-1
    position under `components/`."""

    def test_flat_layout_indexes_identically(self):
        """No-regression assertion: today's flat `components/<name>/`
        layout must resolve exactly as it did pre-leaf-rule."""
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(os.path.realpath(tmp))
            _make_component(root, "bb_core")
            _make_component(root, "bb_num")
            _make_platform(root, "host", "bb_core")
            index = build_index([str(root)])
            self.assertEqual(index.names(), {"bb_core", "bb_num"})
            self.assertEqual(index.component_dir("bb_core"), root / "components" / "bb_core")
            self.assertEqual(index.component_dir("bb_num"), root / "components" / "bb_num")
            self.assertEqual(
                index.platform_dir("bb_core", "host"), root / "platform" / "host" / "bb_core"
            )

    def test_nested_group_resolves_to_leaf_name(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(os.path.realpath(tmp))
            _make_nested_component(root, "wifi", name="bb_wifi_net")
            index = build_index([str(root)])
            self.assertEqual(index.names(), {"bb_wifi_net"})
            self.assertEqual(
                index.component_dir("bb_wifi_net"),
                root / "components" / "wifi" / "bb_wifi_net",
            )

    def test_group_dir_never_itself_a_component(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(os.path.realpath(tmp))
            _make_nested_component(root, "wifi", name="bb_wifi_net")
            index = build_index([str(root)])
            self.assertNotIn("wifi", index.names())

    def test_owner_of_path_attributes_to_leaf_not_group(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(os.path.realpath(tmp))
            _make_nested_component(root, "wifi", name="bb_wifi_net")
            src = root / "components" / "wifi" / "bb_wifi_net" / "src" / "x.c"
            _write(src, "// x\n")
            index = build_index([str(root)])
            self.assertEqual(index.owner_of_path(src), "bb_wifi_net")

    def test_deeper_nesting_two_plus_levels(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(os.path.realpath(tmp))
            _make_nested_component(root, "net", "wifi", name="bb_wifi_deep")
            index = build_index([str(root)])
            self.assertEqual(index.names(), {"bb_wifi_deep"})
            self.assertEqual(
                index.component_dir("bb_wifi_deep"),
                root / "components" / "net" / "wifi" / "bb_wifi_deep",
            )
            self.assertNotIn("net", index.names())
            self.assertNotIn("wifi", index.names())
            src = root / "components" / "net" / "wifi" / "bb_wifi_deep" / "src" / "x.c"
            _write(src, "// x\n")
            self.assertEqual(index.owner_of_path(src), "bb_wifi_deep")

    def test_duplicate_leaf_name_in_different_groups_collides(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(os.path.realpath(tmp))
            _make_nested_component(root, "a", name="bb_dup_leaf")
            _make_nested_component(root, "b", name="bb_dup_leaf")
            with self.assertRaises(CollisionError) as cm:
                build_index([str(root)])
            self.assertIn("bb_dup_leaf", str(cm.exception))

    def test_symlinked_directory_cycle_terminates(self):
        """Review nit: `_leaf_component_dirs`'s recursive walk follows
        symlinks (via `Path.is_dir()`/`iterdir()`), so a symlinked
        directory cycle under `components/` must not recurse unboundedly.
        `components/loop/back` symlinks back to `components/loop` itself,
        the simplest self-cycle; unguarded, this recurses until Python's
        default recursion limit (~1000) trips a `RecursionError` -- it
        does not hang forever. The walk must terminate cleanly instead,
        and still discover a genuine leaf elsewhere in the tree."""
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(os.path.realpath(tmp))
            loop_dir = root / "components" / "loop"
            loop_dir.mkdir(parents=True)
            (loop_dir / "back").symlink_to(loop_dir, target_is_directory=True)
            _make_component(root, "bb_real")
            index = build_index([str(root)])
            self.assertEqual(index.names(), {"bb_real"})

    def test_symlinked_directory_cycle_two_hop(self):
        """Two-directory mutual cycle (`components/a/to_b` ->
        `components/b`, `components/b/to_a` -> `components/a`) — a cycle
        that doesn't revisit the exact starting directory on the very
        first recursive call, unlike the direct self-cycle above."""
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(os.path.realpath(tmp))
            dir_a = root / "components" / "a"
            dir_b = root / "components" / "b"
            dir_a.mkdir(parents=True)
            dir_b.mkdir(parents=True)
            (dir_a / "to_b").symlink_to(dir_b, target_is_directory=True)
            (dir_b / "to_a").symlink_to(dir_a, target_is_directory=True)
            index = build_index([str(root)])
            self.assertEqual(index.names(), set())

    def test_symlinked_diamond_is_not_suppressed_still_collides(self):
        """Regression test for the guard's ancestor-scoping (review
        finding): a non-cyclic symlink DIAMOND -- two sibling dirs
        `components/alias_one` and `components/alias_two` both pointing at
        the SAME real group dir containing a leaf component -- is not a
        cycle at all, and must still surface the pre-existing behavior:
        the same component name discovered twice (once per alias) raises
        `CollisionError`, exactly as it would with the cycle guard absent
        entirely. A global "ever visited realpath" guard would silently
        skip the second alias's subtree instead (no error, only one
        alias's component found) -- precisely the silent-misattribution
        failure mode this whole lane exists to remove, so this asserts
        the loud failure is preserved, not swallowed."""
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(os.path.realpath(tmp))
            real_group = root / "components" / "real_group"
            _make_nested_component(root, "real_group", name="bb_diamond")
            alias_one = root / "components" / "alias_one"
            alias_two = root / "components" / "alias_two"
            alias_one.symlink_to(real_group, target_is_directory=True)
            alias_two.symlink_to(real_group, target_is_directory=True)
            with self.assertRaises(CollisionError) as cm:
                build_index([str(root)])
            self.assertIn("bb_diamond", str(cm.exception))


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
