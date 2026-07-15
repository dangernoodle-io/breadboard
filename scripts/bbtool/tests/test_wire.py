"""wire library-module tests (decision #735, folded into `commands.codegen`):
fixture component tree -> assert emitted bb_app_init.c call order +
http-start-line presence, over synthetic CMakeLists.txt/header fixtures
(never the real breadboard component tree) -- mirrors test_composition.py's
fixture style. CLI (`run()`) coverage lives in test_codegen.py."""
import os
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "commands"))

from commands.wire import WireError, collect_entries, collect_provides_entries, render_source
from wire_graph import MissingProviderError, topo_sort


def _write(path: Path, content: str = "") -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")


def _make_component(root: Path, name: str, header_body: str, requires=None) -> None:
    body = "idf_component_register(\n"
    if requires:
        body += f"    REQUIRES {' '.join(requires)}\n"
    body += ")\n"
    comp = root / "components" / name
    _write(comp / "CMakeLists.txt", body)
    _write(comp / "include" / f"{name}.h", header_body)


def _fixture_root(tmp: str) -> Path:
    """bb_log-alike: stream then config (requires=log_stream), and an
    independent bb_meminfo with no markers at all (no init function)."""
    root = Path(tmp)
    _make_component(
        root, "bb_log",
        "#pragma once\n"
        "// bbtool:init tier=early fn=bb_log_stream_init provides=log_stream\n"
        "bb_err_t bb_log_stream_init(void);\n"
        "// bbtool:init tier=early fn=bb_log_config_init requires=log_stream\n"
        "bb_err_t bb_log_config_init(void);\n",
    )
    _make_component(root, "bb_meminfo", "#pragma once\nbb_err_t bb_meminfo_get(void*);\n")
    return root


def _fixture_root_with_http(tmp: str) -> Path:
    root = _fixture_root(tmp)
    _make_component(
        root, "bb_http",
        "#pragma once\n"
        "// bbtool:init tier=pre_http fn=bb_http_start provides=http_server\n"
        "bb_http_handle_t bb_http_start(void);\n",
    )
    _make_component(
        root, "bb_routes",
        "#pragma once\n"
        "// bbtool:init tier=regular fn=bb_routes_register server=true\n"
        "bb_err_t bb_routes_register(bb_http_handle_t server);\n",
        requires=["bb_http"],
    )
    return root


def _fixture_root_with_consumes(tmp: str, provider: bool, consumer: bool) -> Path:
    """A fake provider (`// bbtool:provides key=demo_sink symbol=bb_example_emit`)
    and/or a fake consumer (`// bbtool:init tier=early fn=bb_example_set_emit
    consumes=demo_sink`), independent of the bb_log/bb_http fixtures above so
    the two paths can never interact."""
    root = Path(tmp)
    if provider:
        _make_component(
            root, "bb_example_provider",
            "#pragma once\n"
            "// bbtool:provides key=demo_sink symbol=bb_example_emit\n"
            "void bb_example_emit(int event);\n",
        )
    if consumer:
        _make_component(
            root, "bb_example_consumer",
            "#pragma once\n"
            "// bbtool:init tier=early fn=bb_example_set_emit consumes=demo_sink\n"
            "void bb_example_set_emit(void (*emit)(int));\n",
        )
    return root


class TestCollectEntries(unittest.TestCase):
    def test_collects_markers_across_components_in_composition_order(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = _fixture_root(tmp)
            entries = collect_entries(str(root), ["bb_log", "bb_meminfo"], "espidf")
            self.assertEqual([e.fn for e in entries], ["bb_log_stream_init", "bb_log_config_init"])

    def test_component_with_no_markers_contributes_nothing(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = _fixture_root(tmp)
            entries = collect_entries(str(root), ["bb_meminfo"], "espidf")
            self.assertEqual(entries, [])


class TestRenderSource(unittest.TestCase):
    def test_early_tier_calls_in_dependency_order(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = _fixture_root(tmp)
            entries = collect_entries(str(root), ["bb_log"], "espidf")
            source = render_source(topo_sort(entries))
            stream_pos = source.index("bb_log_stream_init()")
            config_pos = source.index("bb_log_config_init()")
            self.assertLess(stream_pos, config_pos)
            self.assertIn('#include "bb_log.h"', source)
            self.assertIn("bb_err_t bb_app_init_early(void)", source)
            self.assertIn("bb_err_t bb_app_init_rest(void)", source)
            self.assertIn("bb_err_t bb_app_init(void)", source)

    def test_http_start_line_present_when_http_component_in_set(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = _fixture_root_with_http(tmp)
            entries = collect_entries(str(root), ["bb_log", "bb_http", "bb_routes"], "espidf")
            source = render_source(topo_sort(entries))
            self.assertIn("__auto_type bb_app_http_handle = bb_http_start();", source)
            self.assertIn("bb_routes_register(bb_app_http_handle);", source)

    def test_http_start_line_absent_when_no_http_component(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = _fixture_root(tmp)
            entries = collect_entries(str(root), ["bb_log"], "espidf")
            source = render_source(topo_sort(entries))
            self.assertNotIn("__auto_type", source)

    def test_server_entry_without_http_provider_raises(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = _fixture_root_with_http(tmp)
            entries = collect_entries(str(root), ["bb_routes"], "espidf")
            with self.assertRaises(WireError):
                render_source(topo_sort(entries))

    def test_duplicate_http_server_provider_raises(self):
        """Two components both marking provides=http_server must be a hard
        WireError, not a silent double-call of the second provider."""
        with tempfile.TemporaryDirectory() as tmp:
            root = _fixture_root_with_http(tmp)
            _make_component(
                root, "bb_http2",
                "#pragma once\n"
                "// bbtool:init tier=pre_http fn=bb_http2_start provides=http_server\n"
                "bb_http_handle_t bb_http2_start(void);\n",
            )
            entries = collect_entries(str(root), ["bb_log", "bb_http", "bb_http2", "bb_routes"], "espidf")
            ordered = topo_sort(entries)
            with self.assertRaises(WireError):
                render_source(ordered)

    def test_mistiered_http_server_provider_raises(self):
        """An http_server provider outside tier=pre_http must be a hard
        WireError, not a silent double-call (once as the captured server
        line, once again as a plain call in its own tier)."""
        with tempfile.TemporaryDirectory() as tmp:
            root = _fixture_root(tmp)
            _make_component(
                root, "bb_http_regular",
                "#pragma once\n"
                "// bbtool:init tier=regular fn=bb_http_start provides=http_server\n"
                "bb_http_handle_t bb_http_start(void);\n",
            )
            entries = collect_entries(str(root), ["bb_log", "bb_http_regular"], "espidf")
            ordered = topo_sort(entries)
            with self.assertRaises(WireError):
                render_source(ordered)

    def test_unchanged_http_fixtures_still_pass_with_default_provides(self):
        """Zero-regression guard: the http_server fixture cases must produce
        identical output whether or not `provides_entries` is passed at all
        (the default `()` must be indistinguishable from omitting it)."""
        with tempfile.TemporaryDirectory() as tmp:
            root = _fixture_root_with_http(tmp)
            entries = collect_entries(str(root), ["bb_log", "bb_http", "bb_routes"], "espidf")
            ordered = topo_sort(entries)
            self.assertEqual(render_source(ordered), render_source(ordered, []))


class TestConsumesSetterInjection(unittest.TestCase):
    def test_provider_and_consumer_both_composed_emits_setter_call(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = _fixture_root_with_consumes(tmp, provider=True, consumer=True)
            components = ["bb_example_provider", "bb_example_consumer"]
            entries = collect_entries(str(root), components, "espidf")
            provides = collect_provides_entries(str(root), components, "espidf")
            source = render_source(topo_sort(entries), provides)
            self.assertIn("bb_example_set_emit(bb_example_emit);", source)
            # never routed through the bb_err_t convention
            self.assertNotIn("bb_app_rc = bb_example_set_emit", source)

    def test_only_consumer_composed_drops_entry_silently(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = _fixture_root_with_consumes(tmp, provider=False, consumer=True)
            components = ["bb_example_consumer"]
            entries = collect_entries(str(root), components, "espidf")
            provides = collect_provides_entries(str(root), components, "espidf")
            source = render_source(topo_sort(entries), provides)
            self.assertNotIn("bb_example_set_emit(", source)

    def test_only_provider_composed_is_unused_without_crash(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = _fixture_root_with_consumes(tmp, provider=True, consumer=False)
            components = ["bb_example_provider"]
            entries = collect_entries(str(root), components, "espidf")
            provides = collect_provides_entries(str(root), components, "espidf")
            source = render_source(topo_sort(entries), provides)
            self.assertNotIn("bb_example_emit(", source)

    def test_duplicate_provides_key_raises(self):
        """Two composed components both declaring `key=demo_sink` (with
        different symbols) must be a hard WireError, mirroring the
        http_server path's "at most one provider" check -- never a silent
        last-wins that could wire the wrong setter."""
        with tempfile.TemporaryDirectory() as tmp:
            root = _fixture_root_with_consumes(tmp, provider=True, consumer=True)
            _make_component(
                root, "bb_example_provider2",
                "#pragma once\n"
                "// bbtool:provides key=demo_sink symbol=bb_example2_emit\n"
                "void bb_example2_emit(int event);\n",
            )
            components = ["bb_example_provider", "bb_example_provider2", "bb_example_consumer"]
            entries = collect_entries(str(root), components, "espidf")
            provides = collect_provides_entries(str(root), components, "espidf")
            with self.assertRaises(WireError) as ctx:
                render_source(topo_sort(entries), provides)
            self.assertIn("demo_sink", str(ctx.exception))

    def test_http_server_fixtures_unchanged_by_consumes_path(self):
        """Re-run of the pre-existing http_server assertions, byte-for-byte,
        to prove the setter-injection path adds nothing to that output."""
        with tempfile.TemporaryDirectory() as tmp:
            root = _fixture_root_with_http(tmp)
            entries = collect_entries(str(root), ["bb_log", "bb_http", "bb_routes"], "espidf")
            provides = collect_provides_entries(str(root), ["bb_log", "bb_http", "bb_routes"], "espidf")
            self.assertEqual(provides, [])
            source = render_source(topo_sort(entries), provides)
            self.assertIn("__auto_type bb_app_http_handle = bb_http_start();", source)
            self.assertIn("bb_routes_register(bb_app_http_handle);", source)


def _fixture_root_with_nv_rtc(tmp: str, bogus_provides_key: bool = False) -> Path:
    """Mirrors the REAL bb_nv_config_init/bb_storage_rtc_register markers
    verbatim (B1: bb_nv creds-cluster relocation, requires=storage_rtc) --
    bb_nv "requires" the key bb_storage_rtc "provides", both tier=early.
    bogus_provides_key=True renames the provider's key (typo'd/removed
    provides=) to prove an unmatched requires= is a hard MissingProviderError,
    never a silent same-order-as-input pass-through."""
    root = Path(tmp)
    provides_key = "storage_rtc_TYPO" if bogus_provides_key else "storage_rtc"
    _make_component(
        root, "bb_storage_rtc",
        "#pragma once\n"
        f"// bbtool:init tier=early fn=bb_storage_rtc_register provides={provides_key}\n"
        "bb_err_t bb_storage_rtc_register(void);\n",
    )
    _make_component(
        root, "bb_nv",
        "#pragma once\n"
        "// bbtool:init tier=early fn=bb_nv_config_init requires=storage_rtc\n"
        "bb_err_t bb_nv_config_init(void);\n",
    )
    return root


class TestNvRequiresStorageRtcHostileOrder(unittest.TestCase):
    """B1: bb_nv creds-cluster relocation -- bb_nv_config_init's heal/seed
    reads the shared "rtc" bb_storage backend through bb_settings, which
    needs bb_storage_rtc_register() to have already run in the SAME early
    tier (same-tier order is otherwise unspecified per wire_graph's own
    docstring). Proof mirrors B1-756 PR-B's validation method: a hostile
    parse order (the REQUIRER listed before its PROVIDER in --components)
    must still order the provider first, and an unmatched requires= key must
    hard-fail rather than silently falling back to input order."""

    def test_hostile_parse_order_still_orders_provider_first(self):
        """--components lists bb_nv (the requirer) BEFORE bb_storage_rtc (the
        provider) -- the adversarial order a naive "strip the edge, order
        unchanged" test could not catch, since bb_nv's natural real-repo
        parse order already happens to come after bb_storage_rtc
        alphabetically. This test inverts that natural order on purpose."""
        with tempfile.TemporaryDirectory() as tmp:
            root = _fixture_root_with_nv_rtc(tmp)
            entries = collect_entries(str(root), ["bb_nv", "bb_storage_rtc"], "espidf")
            # Sanity: the hostile order is really present in parse order --
            # otherwise this test would trivially pass without exercising
            # the requires=/provides= edge at all.
            self.assertEqual([e.fn for e in entries],
                              ["bb_nv_config_init", "bb_storage_rtc_register"])

            ordered = topo_sort(entries)
            self.assertEqual([e.fn for e in ordered],
                              ["bb_storage_rtc_register", "bb_nv_config_init"])

    def test_natural_parse_order_also_orders_provider_first(self):
        """Companion case, provider listed first -- both orders converge on
        the same result, proving the edge (not incidental list order) drives
        the outcome."""
        with tempfile.TemporaryDirectory() as tmp:
            root = _fixture_root_with_nv_rtc(tmp)
            entries = collect_entries(str(root), ["bb_storage_rtc", "bb_nv"], "espidf")
            ordered = topo_sort(entries)
            self.assertEqual([e.fn for e in ordered],
                              ["bb_storage_rtc_register", "bb_nv_config_init"])

    def test_bogus_provides_key_hard_fails(self):
        """The provider's key typo'd/renamed away from what bb_nv requires --
        MissingProviderError, never a silent fallback to parse-order (which
        would happen to look "correct" here purely by chance, masking the
        missing edge)."""
        with tempfile.TemporaryDirectory() as tmp:
            root = _fixture_root_with_nv_rtc(tmp, bogus_provides_key=True)
            entries = collect_entries(str(root), ["bb_nv", "bb_storage_rtc"], "espidf")
            with self.assertRaises(MissingProviderError) as ctx:
                topo_sort(entries)
            self.assertIn("storage_rtc", str(ctx.exception))
            self.assertIn("bb_nv_config_init", str(ctx.exception))


if __name__ == "__main__":
    unittest.main()
