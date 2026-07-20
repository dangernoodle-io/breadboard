"""composition resolver tests: component-list-to-REQUIRES transitive
resolution + CMake fragment rendering, over synthetic CMakeLists.txt
fixtures (never the real breadboard component tree) — mirrors
test_boards.py's fixture style. Relocated from the (now-deleted) `bbtool
autowire` CLI's test_autowire.py; `composition.py` has no CLI surface of its
own, so only the resolver/renderer tests survive here — the CLI-only tests
(argparse Namespace wiring, --components validation, run() plumbing) were
deleted along with the command."""
import os
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from boards import ManifestError
from composition import (
    check_format_registry_backends,
    render_cmake_fragment,
    resolve_composition,
    resolve_composition_with_graph,
)


def _write(path: Path, content: str = "") -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")


def _make_component(root: Path, name: str, requires=None, priv_requires=None, src: str = None) -> None:
    body = "idf_component_register(\n"
    if requires:
        body += f"    REQUIRES {' '.join(requires)}\n"
    if priv_requires:
        body += f"    PRIV_REQUIRES {' '.join(priv_requires)}\n"
    body += ")\n"
    comp = root / "components" / name
    _write(comp / "CMakeLists.txt", body)
    _write(comp / "include" / f"{name}.h", "#pragma once\n")
    if src is not None:
        _write(comp / "src" / f"{name}.c", src)


def _fixture_root(tmp: str) -> Path:
    """bb_a <- bb_b <- bb_c chain, plus an independent bb_d, mirroring a small
    slice of the real dependency shape (e.g. bb_wifi -> bb_core)."""
    root = Path(tmp)
    _make_component(root, "bb_a")
    _make_component(root, "bb_b", requires=["bb_a"])
    _make_component(root, "bb_c", priv_requires=["bb_b"])
    _make_component(root, "bb_d")
    return root


class TestResolveComposition(unittest.TestCase):
    def test_resolves_transitive_closure_dependency_before_dependent(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = _fixture_root(tmp)
            order = resolve_composition(str(root), ["bb_c"], platform="espidf")
            self.assertEqual(order, ["bb_a", "bb_b", "bb_c"])

    def test_excludes_components_outside_the_requested_closure(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = _fixture_root(tmp)
            order = resolve_composition(str(root), ["bb_a"], platform="espidf")
            self.assertEqual(order, ["bb_a"])
            self.assertNotIn("bb_b", order)
            self.assertNotIn("bb_c", order)
            self.assertNotIn("bb_d", order)

    def test_unknown_component_raises_manifest_error(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = _fixture_root(tmp)
            with self.assertRaises(ManifestError):
                resolve_composition(str(root), ["bb_ghost"], platform="espidf")

    def test_dedupes_shared_transitive_deps_across_multiple_requested(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = _fixture_root(tmp)
            order = resolve_composition(str(root), ["bb_b", "bb_c"], platform="espidf")
            self.assertEqual(order.count("bb_a"), 1)
            self.assertEqual(order.count("bb_b"), 1)
            self.assertEqual(set(order), {"bb_a", "bb_b", "bb_c"})


class TestRenderCmakeFragment(unittest.TestCase):
    def test_emits_set_with_space_separated_list(self):
        text = render_cmake_fragment(["bb_a", "bb_b", "bb_c"])
        self.assertIn("set(BB_AUTOWIRE_REQUIRES bb_a bb_b bb_c)", text)

    def test_empty_composition_emits_empty_set(self):
        text = render_cmake_fragment([])
        self.assertIn("set(BB_AUTOWIRE_REQUIRES )", text)

    def test_emits_components_allowlist_prefixed_with_main(self):
        text = render_cmake_fragment(["bb_a", "bb_b", "bb_c"])
        self.assertIn("set(BB_AUTOWIRE_COMPONENTS main bb_a bb_b bb_c)", text)

    def test_empty_composition_components_is_just_main(self):
        text = render_cmake_fragment([])
        self.assertIn("set(BB_AUTOWIRE_COMPONENTS main )", text)


_RENDER_CALL_SRC = (
    "#include \"bb_serialize_format.h\"\n"
    "void consume(bb_format_t fmt) { bb_serialize_format_get_render(fmt); }\n"
)
_REGISTER_CALL_SRC = (
    "#include \"bb_serialize_format.h\"\n"
    "void backend_init(void) { bb_serialize_format_register(0, 0); }\n"
)


class TestCheckFormatRegistryBackends(unittest.TestCase):
    """B1-985 (precedent B1-981, reworked to real call-site detection): warn
    when a format-registry consumer -- a component whose C sources actually
    call the registry's render lookup/dispatch entry points -- is composed
    with zero components whose sources call bb_serialize_format_register().
    A mis-composition like this compiles fine, then returns
    BB_ERR_UNSUPPORTED for every format at runtime with no build-time
    signal."""

    def _check(self, root: Path, names):
        components, graph = resolve_composition_with_graph(str(root), names, platform="espidf")
        return check_format_registry_backends(str(root), components, graph)

    def test_consumer_with_zero_backends_warns(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            _make_component(root, "bb_serialize")
            _make_component(root, "bb_consumer", requires=["bb_serialize"], src=_RENDER_CALL_SRC)
            warning = self._check(root, ["bb_serialize", "bb_consumer"])
            self.assertIsNotNone(warning)
            self.assertIn("bb_consumer", warning)
            self.assertIn("bb_serialize_*", warning)

    def test_consumer_with_a_backend_does_not_warn(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            _make_component(root, "bb_serialize")
            _make_component(
                root, "bb_serialize_json", requires=["bb_serialize"], src=_REGISTER_CALL_SRC,
            )
            _make_component(root, "bb_consumer", requires=["bb_serialize"], src=_RENDER_CALL_SRC)
            warning = self._check(root, ["bb_serialize", "bb_serialize_json", "bb_consumer"])
            self.assertIsNone(warning)

    def test_no_consumer_reachable_does_not_warn(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            _make_component(root, "bb_serialize")
            _make_component(root, "bb_unrelated")
            warning = self._check(root, ["bb_serialize", "bb_unrelated"])
            self.assertIsNone(warning)

    def test_registry_not_composed_does_not_warn(self):
        """Guard tested directly against `check_format_registry_backends`
        (not via `resolve_composition_with_graph`, whose closure would
        transitively pull bb_serialize in anyway since bb_consumer REQUIRES
        it) -- a components list that genuinely omits the registry itself
        must short-circuit with no warning."""
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            _make_component(root, "bb_serialize")
            _make_component(root, "bb_consumer", requires=["bb_serialize"], src=_RENDER_CALL_SRC)
            warning = check_format_registry_backends(str(root), ["bb_consumer"], {})
            self.assertIsNone(warning)

    def test_type_only_requirer_does_not_warn(self):
        """A component that REQUIRES bb_serialize purely for the
        bb_serialize_desc_t TYPE (e.g. bb_tcp_client/bb_mqtt_client/
        bb_http_client/bb_meminfo/bb_system in the real tree) but never calls
        a registry render entry point is NOT a consumer -- no warning, even
        with zero backends composed."""
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            _make_component(root, "bb_serialize")
            _make_component(
                root, "bb_type_only_requirer", requires=["bb_serialize"],
                src="#include \"bb_serialize_format.h\"\nconst bb_serialize_desc_t *desc;\n",
            )
            warning = self._check(root, ["bb_serialize", "bb_type_only_requirer"])
            self.assertIsNone(warning)

    def test_non_registering_serialize_prefixed_helper_is_not_a_backend(self):
        """A `bb_serialize_*`-prefixed component that never actually calls
        bb_serialize_format_register() must NOT satisfy the backend check --
        name-prefix alone is insufficient (this is the detection this PR
        replaces)."""
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            _make_component(root, "bb_serialize")
            _make_component(
                root, "bb_serialize_helper", requires=["bb_serialize"],
                src="#include \"bb_serialize_format.h\"\nvoid helper(void) {}\n",
            )
            _make_component(root, "bb_consumer", requires=["bb_serialize"], src=_RENDER_CALL_SRC)
            warning = self._check(root, ["bb_serialize", "bb_serialize_helper", "bb_consumer"])
            self.assertIsNotNone(warning)
            self.assertIn("bb_consumer", warning)

    def test_comment_mentioning_register_does_not_mask_a_real_consumer(self):
        """A component whose C source carries a COMMENT mentioning
        bb_serialize_format_register() (e.g. a docstring cross-reference,
        exactly like platform/espidf/bb_cache_serialize's render path)
        alongside a REAL bb_serialize_format_get_render() call must still be
        classified as a consumer -- comment text must never satisfy the
        backend check or suppress consumer detection. Fails on pre-fix code
        (raw-text scan matches the comment, misclassifying the component as
        a backend and silently swallowing the warning)."""
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            _make_component(root, "bb_serialize")
            _make_component(
                root, "bb_cache_serialize_alike", requires=["bb_serialize"],
                src=(
                    "#include \"bb_serialize_format.h\"\n"
                    "// dispatches through the registry (bb_serialize_format_register()). See\n"
                    "// bb_serialize_format.h for details.\n"
                    "void render(bb_format_t fmt) { bb_serialize_format_get_render(fmt); }\n"
                ),
            )
            warning = self._check(root, ["bb_serialize", "bb_cache_serialize_alike"])
            self.assertIsNotNone(warning)
            self.assertIn("bb_cache_serialize_alike", warning)

    def test_parse_only_consumer_is_detected(self):
        """A component that only calls bb_serialize_format_get_parse()
        (ingest side, no render) is still a consumer -- it fails the same
        silent way at runtime if composed with zero backends."""
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            _make_component(root, "bb_serialize")
            _make_component(
                root, "bb_parse_consumer", requires=["bb_serialize"],
                src=(
                    "#include \"bb_serialize_format.h\"\n"
                    "void ingest(bb_format_t fmt) { bb_serialize_format_get_parse(fmt); }\n"
                ),
            )
            warning = self._check(root, ["bb_serialize", "bb_parse_consumer"])
            self.assertIsNotNone(warning)
            self.assertIn("bb_parse_consumer", warning)


class TestCheckFormatRegistryBackendsMultiRoot(unittest.TestCase):
    """FINDING 4 (B1-1084 review): the multi-root owning-root resolution in
    `check_format_registry_backends` (`index.entry(name)`, `entry_root !=
    primary_root`) had no dedicated test -- every case above passes a bare
    single-root string. A format-registry consumer/backend discovered under
    a NON-primary root must have its C sources read from ITS OWN root, not
    blindly from the primary root (where the file doesn't exist at all --
    a stale primary-root read would silently see empty text, misclassifying
    the component and suppressing the warning it should raise)."""

    def test_consumer_under_non_primary_root_without_backend_warns(self):
        with tempfile.TemporaryDirectory() as tmp_a, tempfile.TemporaryDirectory() as tmp_b:
            root_a, root_b = Path(tmp_a), Path(tmp_b)
            _make_component(root_a, "bb_serialize")
            _make_component(root_b, "bb_consumer", requires=["bb_serialize"], src=_RENDER_CALL_SRC)
            roots = [str(root_a), str(root_b)]
            components, graph = resolve_composition_with_graph(
                roots, ["bb_serialize", "bb_consumer"], platform="espidf"
            )
            warning = check_format_registry_backends(roots, components, graph)
            self.assertIsNotNone(warning)
            self.assertIn("bb_consumer", warning)

    def test_backend_under_non_primary_root_suppresses_warning(self):
        with tempfile.TemporaryDirectory() as tmp_a, tempfile.TemporaryDirectory() as tmp_b:
            root_a, root_b = Path(tmp_a), Path(tmp_b)
            _make_component(root_a, "bb_serialize")
            _make_component(
                root_b, "bb_serialize_json", requires=["bb_serialize"], src=_REGISTER_CALL_SRC,
            )
            _make_component(root_b, "bb_consumer", requires=["bb_serialize"], src=_RENDER_CALL_SRC)
            roots = [str(root_a), str(root_b)]
            components, graph = resolve_composition_with_graph(
                roots, ["bb_serialize", "bb_serialize_json", "bb_consumer"], platform="espidf"
            )
            warning = check_format_registry_backends(roots, components, graph)
            self.assertIsNone(warning)


if __name__ == "__main__":
    unittest.main()
