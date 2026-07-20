"""boards.py tests: capability/board resolve, component-graph derivation,
transitive BFS, and manifest validation — over synthetic CMakeLists.txt +
directory-tree fixtures (never the real breadboard component tree)."""
import os
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from boards import (
    ManifestError,
    build_graph,
    derive_component,
    discover_components,
    load_manifest,
    resolve_active_capabilities,
    resolve_component_names,
    resolve_transitive,
)


def _write(path: Path, content: str = "") -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")


def _make_component(root: Path, name: str, cmake_body: str = "idf_component_register()\n",
                     include_header: bool = True, src_files=None, flat_files=None) -> None:
    """Synthetic components/<name>/ fixture."""
    comp = root / "components" / name
    _write(comp / "CMakeLists.txt", cmake_body)
    if include_header:
        _write(comp / "include" / f"{name}.h", "#pragma once\n")
    for f in (src_files or []):
        _write(comp / "src" / f, "// src\n")
    for f in (flat_files or []):
        _write(comp / f, "// flat\n")


def _make_platform(root: Path, layer: str, name: str, files=None, under_include: bool = False) -> None:
    base = root / "platform" / layer / name
    for f in (files or []):
        if under_include:
            _write(base / "include" / f, "// hdr\n")
        else:
            _write(base / f, "// impl\n")


class TestManifestLoad(unittest.TestCase):
    def test_load_manifest_splits_capability_and_board(self):
        config = {
            "capability": {"core": {"required": True}, "psram": {}},
            "board": {"native": {"platform": "host"}},
        }
        caps, boards = load_manifest(config)
        self.assertEqual(set(caps), {"core", "psram"})
        self.assertEqual(set(boards), {"native"})

    def test_load_manifest_empty_config(self):
        caps, boards = load_manifest({})
        self.assertEqual(caps, {})
        self.assertEqual(boards, {})


class TestResolveActiveCapabilities(unittest.TestCase):
    def setUp(self):
        self.capabilities = {
            "core": {"required": True, "components": ["bb_core"]},
            "psram": {"sdkconfig": {"CONFIG_SPIRAM": "y"}},
            "asic": {"add_components": ["asic_bm1370"]},
        }
        self.boards = {
            "bitaxe": {"platform": "espidf", "capabilities": ["psram", "asic"]},
            "cyd": {"platform": "espidf", "capabilities": []},
        }

    def test_required_union_listed(self):
        active = resolve_active_capabilities("bitaxe", self.boards, self.capabilities)
        self.assertEqual(active, ["asic", "core", "psram"])

    def test_required_always_included_even_when_not_listed(self):
        active = resolve_active_capabilities("cyd", self.boards, self.capabilities)
        self.assertEqual(active, ["core"])

    def test_unknown_board_raises(self):
        with self.assertRaises(ManifestError):
            resolve_active_capabilities("nope", self.boards, self.capabilities)

    def test_unknown_capability_in_board_list_raises(self):
        boards = {"bad": {"capabilities": ["typo_cap"]}}
        with self.assertRaises(ManifestError):
            resolve_active_capabilities("bad", boards, self.capabilities)


class TestResolveComponentNames(unittest.TestCase):
    def test_union_add_remove(self):
        capabilities = {
            "core": {"required": True, "components": ["bb_core", "bb_log"]},
            "display": {"add_components": ["bb_display"]},
        }
        boards = {
            "cyd": {
                "capabilities": ["display"],
                "add_components": ["bb_extra"],
                "remove_components": ["bb_log"],
            }
        }
        names = resolve_component_names("cyd", boards, capabilities)
        self.assertEqual(names, ["bb_core", "bb_display", "bb_extra"])

    def test_required_only_board_gets_core_components(self):
        capabilities = {"core": {"required": True, "components": ["bb_core", "bb_str"]}}
        boards = {"native": {}}
        names = resolve_component_names("native", boards, capabilities)
        self.assertEqual(names, ["bb_core", "bb_str"])

    def test_two_boards_sharing_capability_diverge_via_add_components(self):
        """B1-747: two boards both activating the SAME capability (mirroring
        smoke's shared [capability.smoke]) resolve to DIFFERENT component
        sets purely from one board's own `add_components` -- the mechanism
        that replaces smoke's hand-written board-conditional
        `list(APPEND SMOKE_REQUIRES ...)` groups."""
        capabilities = {
            "smoke": {"components": ["bb_nv", "bb_log", "bb_wifi"]},
        }
        boards = {
            "smoke_plain": {"capabilities": ["smoke"]},
            "smoke_display": {
                "capabilities": ["smoke"],
                "add_components": ["bb_display", "bb_display_info"],
            },
        }
        plain = resolve_component_names("smoke_plain", boards, capabilities)
        display = resolve_component_names("smoke_display", boards, capabilities)
        self.assertEqual(plain, ["bb_log", "bb_nv", "bb_wifi"])
        self.assertEqual(
            display, ["bb_display", "bb_display_info", "bb_log", "bb_nv", "bb_wifi"]
        )
        self.assertNotEqual(plain, display)

    def test_unknown_board_raises_manifest_error(self):
        capabilities = {"core": {"required": True, "components": ["bb_core"]}}
        boards = {"cyd": {}}
        with self.assertRaises(ManifestError):
            resolve_component_names("nope", boards, capabilities)


class TestDiscoverComponents(unittest.TestCase):
    def test_discovers_components_and_platform_dirs(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            _make_component(root, "bb_core")
            _make_platform(root, "host", "bb_core", files=["bb_core.c"])
            _make_platform(root, "espidf", "bb_only_platform", files=["x.c"])
            universe = discover_components(str(root))
            self.assertEqual(universe, {"bb_core", "bb_only_platform"})

    def test_empty_tree(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.assertEqual(discover_components(tmp), set())


class TestDeriveComponent(unittest.TestCase):
    def test_include_src_and_platform_layer(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            _make_component(
                root, "bb_foo",
                cmake_body="idf_component_register(REQUIRES bb_core PRIV_REQUIRES bb_log)\n",
                src_files=["bb_foo_a.c", "bb_foo_b.c"],
            )
            _make_platform(root, "host", "bb_foo", files=["bb_foo_host.c"])
            entry = derive_component(str(root), "bb_foo", "host")
            self.assertEqual(entry["depends"], ["bb_core", "bb_log"])
            self.assertIn("components/bb_foo/include", entry["includes"])
            self.assertIn("components/bb_foo/src", entry["includes"])
            self.assertIn("platform/host/bb_foo", entry["includes"])
            self.assertEqual(
                sorted(entry["sources"]),
                sorted([
                    "components/bb_foo/src/bb_foo_a.c",
                    "components/bb_foo/src/bb_foo_b.c",
                    "platform/host/bb_foo/bb_foo_host.c",
                ]),
            )

    def test_flat_layout_no_src_dir(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            _make_component(root, "bb_diag", flat_files=["bb_diag_a.c"])
            entry = derive_component(str(root), "bb_diag", "host")
            self.assertIn("components/bb_diag", entry["includes"])
            self.assertEqual(entry["sources"], ["components/bb_diag/bb_diag_a.c"])

    def test_platform_layer_with_include_subdir(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            _make_component(root, "bb_bar")
            _make_platform(root, "espidf", "bb_bar", files=["bb_bar.c"], under_include=False)
            (root / "platform" / "espidf" / "bb_bar" / "include").mkdir(parents=True)
            entry = derive_component(str(root), "bb_bar", "espidf")
            self.assertIn("platform/espidf/bb_bar/include", entry["includes"])

    def test_test_subtree_excluded_from_sources(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            comp = root / "components" / "bb_foo"
            _write(comp / "CMakeLists.txt", "idf_component_register()\n")
            _write(comp / "src" / "bb_foo.c", "// real\n")
            _write(comp / "src" / "test" / "test_bb_foo.c", "// test only\n")
            entry = derive_component(str(root), "bb_foo", "host")
            self.assertEqual(entry["sources"], ["components/bb_foo/src/bb_foo.c"])

    def test_scaffold_hint_adds_residual_include_and_source(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            cmake = (
                "idf_component_register(REQUIRES bb_core)\n"
                "# bbtool-scaffold-hint: include=components/bb_foo/legacy\n"
                "# bbtool-scaffold-hint: source=components/bb_foo/legacy/shim.c\n"
            )
            _make_component(root, "bb_foo", cmake_body=cmake)
            entry = derive_component(str(root), "bb_foo", "host")
            self.assertIn("components/bb_foo/legacy", entry["includes"])
            self.assertIn("components/bb_foo/legacy/shim.c", entry["sources"])

    def test_no_cmakelists_yields_empty_depends(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "components" / "bb_bare" / "include").mkdir(parents=True)
            entry = derive_component(str(root), "bb_bare", "host")
            self.assertEqual(entry["depends"], [])

    def test_unknown_name_empty_roots_falls_back_to_dot(self):
        """#3: `entry is None` guard, empty-`roots` branch — a name absent
        from the index with NO roots at all falls back to the literal '.'
        owning root. Unreachable from any real call site (every real caller
        passes a non-empty `roots`), but the fallback must not raise."""
        entry = derive_component([], "bb_ghost", "host")
        self.assertEqual(entry, {"includes": [], "sources": [], "depends": []})

    def test_unknown_name_nonempty_roots_falls_back_to_first_root(self):
        """#3: `entry is None` guard, non-empty-`roots` branch — a name
        absent from the index falls back to `roots[0]` rather than raising.
        Also unreachable from a real call site (a name absent from every
        scanned root never reaches `derive_component` at all — callers
        filter against `discover_components`'s universe first), but the
        fallback must not raise."""
        with tempfile.TemporaryDirectory() as tmp:
            entry = derive_component([tmp], "bb_ghost", "host")
            self.assertEqual(entry, {"includes": [], "sources": [], "depends": []})

    def test_platform_only_component_depends_are_read_from_platform_cmakelists(self):
        # B1-903: a platform-only component (no components/<name>/ dir at
        # all -- e.g. bb_event_routes_espidf) declares its own
        # idf_component_register(...) directly under
        # platform/<platform>/<name>/CMakeLists.txt. Ignoring that file
        # dropped its REQUIRES/PRIV_REQUIRES from the derived closure
        # entirely, making anything depended on ONLY via that layer (e.g.
        # bb_sse_writer) look dead to the resolver.
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            _write(
                root / "platform" / "espidf" / "bb_routes_espidf" / "CMakeLists.txt",
                "idf_component_register(\n"
                "    SRCS \"bb_routes_espidf.c\"\n"
                "    REQUIRES bb_core\n"
                "    PRIV_REQUIRES bb_sse_writer bb_log)\n",
            )
            entry = derive_component(str(root), "bb_routes_espidf", "espidf")
            self.assertEqual(entry["depends"], ["bb_core", "bb_log", "bb_sse_writer"])

    def test_component_and_platform_layer_depends_are_unioned(self):
        # A component that ALSO has a components/<name>/ CMakeLists.txt
        # (declaring its own REQUIRES) must union both layers' depends,
        # not just one.
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            _make_component(
                root, "bb_foo",
                cmake_body="idf_component_register(REQUIRES bb_core PRIV_REQUIRES bb_log)\n",
            )
            _write(
                root / "platform" / "espidf" / "bb_foo" / "CMakeLists.txt",
                "idf_component_register(PRIV_REQUIRES bb_task)\n",
            )
            entry = derive_component(str(root), "bb_foo", "espidf")
            self.assertEqual(entry["depends"], ["bb_core", "bb_log", "bb_task"])

    def test_platform_layer_with_no_cmakelists_still_has_empty_depends(self):
        # No CMakeLists.txt at either layer -> empty depends, same as
        # before this fix (regression guard for the no-op case).
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            _make_platform(root, "host", "bb_bare_platform", files=["x.c"])
            entry = derive_component(str(root), "bb_bare_platform", "host")
            self.assertEqual(entry["depends"], [])


class TestResolveTransitive(unittest.TestCase):
    def test_dependency_before_dependent_order(self):
        graph = {
            "a": {"depends": ["b"]},
            "b": {"depends": ["c"]},
            "c": {"depends": []},
        }
        order = resolve_transitive(["a"], graph, universe={"a", "b", "c"})
        self.assertEqual(order, ["c", "b", "a"])

    def test_cycle_tolerated(self):
        graph = {
            "a": {"depends": ["b"]},
            "b": {"depends": ["a"]},
        }
        order = resolve_transitive(["a"], graph, universe={"a", "b"})
        self.assertEqual(set(order), {"a", "b"})
        self.assertEqual(len(order), 2)

    def test_unknown_dependency_silently_filtered(self):
        # A depend name outside the universe (e.g. an ESP-IDF SDK component
        # like esp_timer) is not a manifest typo — it's just not
        # scaffold-resolvable, so it's skipped rather than erroring.
        graph = {"a": {"depends": ["esp_timer"]}}
        order = resolve_transitive(["a"], graph, universe={"a"})
        self.assertEqual(order, ["a"])

    def test_unknown_requested_raises(self):
        with self.assertRaises(ManifestError):
            resolve_transitive(["ghost"], {}, universe=set())


class TestBuildGraph(unittest.TestCase):
    def _fixture_root(self, tmp: str) -> Path:
        root = Path(tmp)
        _make_component(root, "bb_core")
        _make_platform(root, "host", "bb_core", files=["bb_core.c"])
        _make_component(
            root, "bb_log",
            cmake_body="idf_component_register(REQUIRES bb_core)\n",
        )
        _make_platform(root, "host", "bb_log", files=["bb_log.c"])
        _make_component(
            root, "bb_display",
            cmake_body="idf_component_register(PRIV_REQUIRES bb_log)\n",
        )
        _make_platform(root, "host", "bb_display", files=["bb_display.c"])
        return root

    def test_full_resolution_end_to_end(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = self._fixture_root(tmp)
            config = {
                "capability": {
                    "core": {"required": True, "components": ["bb_core", "bb_log"]},
                    "display": {"add_components": ["bb_display"]},
                },
                "board": {"cyd": {"platform": "host", "capabilities": ["display"]}},
            }
            graph = build_graph(str(root), "cyd", config)
            self.assertEqual(graph["capabilities"], ["core", "display"])
            self.assertEqual(graph["order"], ["bb_core", "bb_log", "bb_display"])
            self.assertEqual(graph["components"]["bb_display"]["depends"], ["bb_log"])

    def test_unknown_board_raises(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = self._fixture_root(tmp)
            with self.assertRaises(ManifestError):
                build_graph(str(root), "ghost", {"board": {}, "capability": {}})

    def test_manifest_component_typo_raises(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = self._fixture_root(tmp)
            config = {
                "capability": {"core": {"required": True, "components": ["bb_core", "bb_typo"]}},
                "board": {"native": {"platform": "host"}},
            }
            with self.assertRaises(ManifestError):
                build_graph(str(root), "native", config)

    def test_external_sdk_depends_filtered_out(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = self._fixture_root(tmp)
            _make_component(
                root, "bb_needs_sdk",
                cmake_body="idf_component_register(PRIV_REQUIRES bb_core esp_timer)\n",
            )
            config = {
                "capability": {"core": {"required": True, "components": ["bb_needs_sdk"]}},
                "board": {"native": {"platform": "host"}},
            }
            graph = build_graph(str(root), "native", config)
            self.assertEqual(graph["components"]["bb_needs_sdk"]["depends"], ["bb_core"])
            self.assertNotIn("esp_timer", graph["components"])

    def test_deterministic_across_runs(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = self._fixture_root(tmp)
            config = {
                "capability": {"core": {"required": True, "components": ["bb_core", "bb_log", "bb_display"]}},
                "board": {"native": {"platform": "host"}},
            }
            g1 = build_graph(str(root), "native", config)
            g2 = build_graph(str(root), "native", config)
            self.assertEqual(g1, g2)


class TestRealTreeSmokeAndFloorBoards(unittest.TestCase):
    """B1-747: every existing composition test above uses synthetic
    fixtures only. Resolve every real smoke/floor manifest board id against
    the REAL repo tree instead -- the net that catches a typo'd/renamed/
    dropped component in bbtool.toml's [capability.smoke]/[board.*] tables,
    exactly the drift class this ticket closes. A ManifestError here means
    the manifest references a component that doesn't actually exist under
    components/ or platform/{host,espidf,arduino}/."""

    ROOT = str(Path(__file__).resolve().parents[3])

    def _config(self) -> dict:
        from core import load_config
        return load_config(None, self.ROOT)

    def test_every_smoke_and_floor_board_resolves_without_error(self):
        config = self._config()
        _, boards = load_manifest(config)
        board_ids = [
            name for name in boards
            if name == "floor" or "smoke" in boards[name].get("capabilities", [])
        ]
        # Sanity: the manifest actually declares the boards this ticket adds
        # (fails loudly if bbtool.toml's smoke/floor tables get renamed out
        # from under this test rather than silently resolving zero boards).
        self.assertGreaterEqual(len(board_ids), 5)
        for board_id in sorted(board_ids):
            with self.subTest(board=board_id):
                graph = build_graph(self.ROOT, board_id, config)
                self.assertTrue(graph["order"])


if __name__ == "__main__":
    unittest.main()
