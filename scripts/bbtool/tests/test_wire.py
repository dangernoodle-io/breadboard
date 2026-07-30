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

from commands.wire import (
    WireError,
    _component_headers,
    collect_entries,
    collect_manifest_entries,
    collect_provides_entries,
    render_source,
)
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


def _fixture_root_with_wildcard(tmp: str, route_order=None, wildcard_order=None) -> Path:
    """B1-1280: an http_server provider (bb_http) + a server=true consumer
    route (bb_routes, order= configurable) + an entry marking
    provides=http_wildcard_last (bb_wildcard, order= configurable) --
    isolated from _fixture_root_with_http so route/wildcard order= can be
    controlled precisely per test case."""
    root = _fixture_root(tmp)
    _make_component(
        root, "bb_http",
        "#pragma once\n"
        "// bbtool:init tier=pre_http fn=bb_http_start provides=http_server\n"
        "bb_http_handle_t bb_http_start(void);\n",
    )
    route_marker = "// bbtool:init tier=regular fn=bb_routes_register server=true"
    if route_order is not None:
        route_marker += f" order={route_order}"
    _make_component(
        root, "bb_routes",
        "#pragma once\n" + route_marker + "\n"
        "bb_err_t bb_routes_register(bb_http_handle_t server);\n",
        requires=["bb_http"],
    )
    wildcard_marker = "// bbtool:init tier=regular fn=bb_wildcard_register provides=http_wildcard_last"
    if wildcard_order is not None:
        wildcard_marker += f" order={wildcard_order}"
    _make_component(
        root, "bb_wildcard",
        "#pragma once\n" + wildcard_marker + "\n"
        "bb_err_t bb_wildcard_register(void);\n",
    )
    return root


def _fixture_root_with_wildcard_args_route(tmp: str, route_order=None, wildcard_order=None,
                                            registers_routes: bool = True) -> Path:
    """B1-1280 blind-spot-closure fixture: an http_server provider (bb_http)
    + an `args=`-shaped route-registering consumer (bb_args_route, order=
    configurable, `registers_routes=true` by default -- pass False to prove
    a plain args= entry with no opt-in is unaffected) + an entry marking
    provides=http_wildcard_last (bb_wildcard, order= configurable). Mirrors
    _fixture_root_with_wildcard, but the consumer registers routes via
    `args=` (referencing bb_app_http_handle directly) instead of
    `server=true` -- the exact shape jae/smoke-compose-routes composes."""
    root = _fixture_root(tmp)
    _make_component(
        root, "bb_http",
        "#pragma once\n"
        "// bbtool:init tier=pre_http fn=bb_http_start provides=http_server\n"
        "bb_http_handle_t bb_http_start(void);\n",
    )
    route_marker = (
        "// bbtool:init tier=regular fn=bb_args_route_register "
        'args=bb_app_http_handle,"/ping"'
    )
    if registers_routes:
        route_marker += " registers_routes=true"
    if route_order is not None:
        route_marker += f" order={route_order}"
    _make_component(
        root, "bb_args_route",
        "#pragma once\n" + route_marker + "\n"
        "bb_err_t bb_args_route_register(bb_http_handle_t server, const char *path);\n",
        requires=["bb_http"],
    )
    wildcard_marker = "// bbtool:init tier=regular fn=bb_wildcard_register provides=http_wildcard_last"
    if wildcard_order is not None:
        wildcard_marker += f" order={wildcard_order}"
    _make_component(
        root, "bb_wildcard",
        "#pragma once\n" + wildcard_marker + "\n"
        "bb_err_t bb_wildcard_register(void);\n",
    )
    return root


def _fixture_root_with_consumes(tmp: str, provider: bool, consumer: bool,
                                ctx: str = None) -> Path:
    """A fake provider (`// bbtool:provides key=demo_sink symbol=bb_example_emit`)
    and/or a fake consumer (`// bbtool:init tier=early fn=bb_example_set_emit
    consumes=demo_sink`), independent of the bb_log/bb_http fixtures above so
    the two paths can never interact. `ctx` optionally appends `ctx=<expr>`
    to the consumer marker (B1-1045 PR-1)."""
    root = Path(tmp)
    if provider:
        _make_component(
            root, "bb_example_provider",
            "#pragma once\n"
            "// bbtool:provides key=demo_sink symbol=bb_example_emit\n"
            "void bb_example_emit(int event);\n",
        )
    if consumer:
        ctx_suffix = f" ctx={ctx}" if ctx else ""
        _make_component(
            root, "bb_example_consumer",
            "#pragma once\n"
            f"// bbtool:init tier=early fn=bb_example_set_emit consumes=demo_sink{ctx_suffix}\n"
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


class TestComponentFieldRejectedInComponentHeader(unittest.TestCase):
    """B1-1275: 'component=' is only valid on a --consumer-manifest marker --
    collect_entries (component headers) must hard-error, never silently
    accept it (a component header already has an owning component; letting
    it name ANOTHER component would be un-fenced implicit composition)."""

    def test_component_field_in_component_header_is_wire_error(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            _make_component(
                root, "bb_sneaky",
                "#pragma once\n"
                "// bbtool:init tier=early fn=bb_sneaky_init component=bb_other\n"
                "bb_err_t bb_sneaky_init(void);\n",
            )
            with self.assertRaises(WireError):
                collect_entries(str(root), ["bb_sneaky"], "espidf")


class TestOutFieldRejectedInComponentHeader(unittest.TestCase):
    """out= mirrors component='s manifest-only restriction: an out-param
    handle is consumer-owned state, not a constant intrinsic to the
    component, so collect_entries hard-errors on it exactly like it does
    for component=."""

    def test_out_field_in_component_header_is_wire_error(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            _make_component(
                root, "bb_sneaky_out",
                "#pragma once\n"
                "// bbtool:init tier=early fn=bb_sneaky_init out=s_binding:int\n"
                "bb_err_t bb_sneaky_init(void);\n",
            )
            with self.assertRaises(WireError):
                collect_entries(str(root), ["bb_sneaky_out"], "espidf")


class TestComponentHeadersUnknownName(unittest.TestCase):
    """#3: `entry is None` guard in `_component_headers` -- a name absent
    from the discovery index falls back to `roots[0]` (or `""` when `roots`
    is also empty) rather than raising. Unreachable from any real call site
    (every real caller passes a name already validated against the
    discovered universe), but the fallback must not raise either way."""

    def test_unknown_name_empty_roots_returns_no_headers(self):
        self.assertEqual(_component_headers([], "bb_ghost", "espidf"), [])

    def test_unknown_name_nonempty_roots_returns_no_headers(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.assertEqual(_component_headers([tmp], "bb_ghost", "espidf"), [])


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

    def test_http_server_provider_with_requires_raises(self):
        """B1-853: a server=true/provides=http_server entry's __auto_type
        capture line can never be conditionally skipped while still
        producing the typed handle every downstream server=true call
        depends on -- an http_server provider that also declares
        `requires=` must be a hard build-time WireError, not a silent
        unguarded-capture gap."""
        with tempfile.TemporaryDirectory() as tmp:
            root = _fixture_root_with_http(tmp)
            _make_component(
                root, "bb_http_dep",
                "#pragma once\n"
                "// bbtool:init tier=early fn=bb_http_dep_init provides=http_dep\n"
                "bb_err_t bb_http_dep_init(void);\n",
            )
            _make_component(
                root, "bb_http_gated",
                "#pragma once\n"
                "// bbtool:init tier=pre_http fn=bb_http_gated_start "
                "provides=http_server requires=http_dep\n"
                "bb_http_handle_t bb_http_gated_start(void);\n",
            )
            entries = collect_entries(
                str(root), ["bb_log", "bb_http_dep", "bb_http_gated", "bb_routes"], "espidf")
            ordered = topo_sort(entries)
            with self.assertRaises(WireError) as ctx:
                render_source(ordered)
            self.assertIn("bb_http_gated_start", str(ctx.exception))
            self.assertIn("requires=", str(ctx.exception))

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
            # No ctx= on the marker -> NULL default (B1-1045 PR-1).
            self.assertIn("bb_example_set_emit(bb_example_emit, NULL);", source)
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

    def test_explicit_ctx_expr_emitted_in_place_of_null(self):
        """A marker with `ctx=<expr>` (B1-1045 PR-1) emits that expression
        as the setter's second argument instead of the NULL default."""
        with tempfile.TemporaryDirectory() as tmp:
            root = _fixture_root_with_consumes(tmp, provider=True, consumer=True,
                                               ctx="&s_binding")
            components = ["bb_example_provider", "bb_example_consumer"]
            entries = collect_entries(str(root), components, "espidf")
            provides = collect_provides_entries(str(root), components, "espidf")
            source = render_source(topo_sort(entries), provides)
            self.assertIn("bb_example_set_emit(bb_example_emit, &s_binding);", source)
            self.assertNotIn("bb_example_set_emit(bb_example_emit, NULL);", source)

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


def _fixture_root_with_gate_chain(tmp: str) -> Path:
    """A `provides=x`, B `requires=x provides=y`, C `requires=y` -- a 3-hop
    chain (B1-853) proving: (1) B is gated on A's success, (2) C is gated on
    B's success (skip propagation: B skipped -> B never marks y available ->
    C is also skipped), all tier=early. A plain, unrelated `fn=bb_d_noop`
    entry with NO `requires=` is included to prove an unguarded entry emits
    byte-identically to the pre-gating call form."""
    root = Path(tmp)
    _make_component(
        root, "bb_a",
        "#pragma once\n"
        "// bbtool:init tier=early fn=bb_a_init provides=x\n"
        "bb_err_t bb_a_init(void);\n",
    )
    _make_component(
        root, "bb_b",
        "#pragma once\n"
        "// bbtool:init tier=early fn=bb_b_init requires=x provides=y\n"
        "bb_err_t bb_b_init(void);\n",
    )
    _make_component(
        root, "bb_c",
        "#pragma once\n"
        "// bbtool:init tier=early fn=bb_c_init requires=y\n"
        "bb_err_t bb_c_init(void);\n",
    )
    _make_component(
        root, "bb_d",
        "#pragma once\n"
        "// bbtool:init tier=early fn=bb_d_noop\n"
        "bb_err_t bb_d_noop(void);\n",
    )
    return root


class TestGateDependents(unittest.TestCase):
    """B1-853: a `requires=` entry must be SKIPPED (not called) when its
    required token's provider hasn't (yet) succeeded, rather than running
    unconditionally against a possibly-broken substrate."""

    def test_dependent_is_guarded_on_providers_availability_flag(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = _fixture_root_with_gate_chain(tmp)
            entries = collect_entries(str(root), ["bb_a", "bb_b", "bb_c", "bb_d"], "espidf")
            source = render_source(topo_sort(entries))
            self.assertIn("static bool bb_app_avail_x = false;", source)
            self.assertIn(
                "    if (bb_app_avail_x) {\n"
                "        bb_app_rc = bb_b_init();\n"
                "        if (bb_app_rc != BB_OK && bb_app_first_err == BB_OK) "
                "{ bb_app_first_err = bb_app_rc; }\n"
                "        if (bb_app_rc == BB_OK) {\n"
                "            bb_app_avail_y = true;\n"
                "        }\n"
                "    } else {\n"
                '        bb_log_w(BB_APP_INIT_TAG, "skipping bb_b_init: '
                'required provider unavailable");\n'
                "    }\n",
                source,
            )
            # A's success marks x available -- unguarded (A has no requires=).
            self.assertIn(
                "    bb_app_rc = bb_a_init();\n"
                "    if (bb_app_rc != BB_OK && bb_app_first_err == BB_OK) "
                "{ bb_app_first_err = bb_app_rc; }\n"
                "    if (bb_app_rc == BB_OK) {\n"
                "        bb_app_avail_x = true;\n"
                "    }\n",
                source,
            )

    def test_skip_propagates_transitively_to_downstream_dependent(self):
        """C `requires=y`, which only bb_b_init `provides=`; the SAME
        availability-guard machinery gates C on `bb_app_avail_y`, which only
        becomes true if bb_b_init actually ran and succeeded -- so a skipped
        (never-called) bb_b_init transitively skips bb_c_init too, with no
        special-cased propagation logic needed."""
        with tempfile.TemporaryDirectory() as tmp:
            root = _fixture_root_with_gate_chain(tmp)
            entries = collect_entries(str(root), ["bb_a", "bb_b", "bb_c", "bb_d"], "espidf")
            source = render_source(topo_sort(entries))
            self.assertIn(
                "    if (bb_app_avail_y) {\n"
                "        bb_app_rc = bb_c_init();\n"
                "        if (bb_app_rc != BB_OK && bb_app_first_err == BB_OK) "
                "{ bb_app_first_err = bb_app_rc; }\n"
                "    } else {\n"
                '        bb_log_w(BB_APP_INIT_TAG, "skipping bb_c_init: '
                'required provider unavailable");\n'
                "    }\n",
                source,
            )

    def test_entry_with_no_requires_emits_byte_identical_unguarded_call(self):
        """bb_d_noop has no `requires=` -- its call must be emitted exactly
        as the pre-gating convention (no guard, no availability bookkeeping),
        even though OTHER entries in the same file are gated."""
        with tempfile.TemporaryDirectory() as tmp:
            root = _fixture_root_with_gate_chain(tmp)
            entries = collect_entries(str(root), ["bb_a", "bb_b", "bb_c", "bb_d"], "espidf")
            source = render_source(topo_sort(entries))
            self.assertIn(
                "    bb_app_rc = bb_d_noop();\n"
                "    if (bb_app_rc != BB_OK && bb_app_first_err == BB_OK) "
                "{ bb_app_first_err = bb_app_rc; }\n\n",
                source,
            )
            self.assertNotIn("bb_app_avail_", source.split("bb_app_rc = bb_d_noop();")[1].split("\n\n")[0])

    def test_no_requires_anywhere_emits_no_gating_scaffolding_at_all(self):
        """Zero-regression guard: a composition with no `requires=` markers
        at all gets NO availability flags, NO extra includes, NO guard -- the
        whole-file output stays byte-identical to pre-B1-853 codegen."""
        with tempfile.TemporaryDirectory() as tmp:
            root = _fixture_root(tmp)
            entries = collect_entries(str(root), ["bb_meminfo"], "espidf")
            source = render_source(topo_sort(entries))
            self.assertNotIn("bb_app_avail_", source)
            self.assertNotIn("<stdbool.h>", source)
            self.assertNotIn("BB_APP_INIT_TAG", source)


def _fixture_root_with_multi_provider(tmp: str) -> Path:
    """Two independent entries both `provides=x` (e.g. two backends of the
    same capability), and a single dependent `requires=x` -- proves
    availability is the OR of every provider's success: EITHER provider
    succeeding must be enough to un-gate the dependent, and the dependent
    must be ordered after BOTH providers (never interleaved between them)."""
    root = Path(tmp)
    _make_component(
        root, "bb_p1",
        "#pragma once\n"
        "// bbtool:init tier=early fn=bb_p1_init provides=x\n"
        "bb_err_t bb_p1_init(void);\n",
    )
    _make_component(
        root, "bb_p2",
        "#pragma once\n"
        "// bbtool:init tier=early fn=bb_p2_init provides=x\n"
        "bb_err_t bb_p2_init(void);\n",
    )
    _make_component(
        root, "bb_dep",
        "#pragma once\n"
        "// bbtool:init tier=early fn=bb_dep_init requires=x\n"
        "bb_err_t bb_dep_init(void);\n",
    )
    return root


class TestGateDependentsMultiProvider(unittest.TestCase):
    """B1-853: multi-provider token -- availability is the OR of every
    provides= entry's success (each provider only ever sets its flag to
    `true`, never resets it), never last-writer-wins or first-writer-only."""

    def test_dependent_guarded_on_single_flag_set_by_either_provider(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = _fixture_root_with_multi_provider(tmp)
            entries = collect_entries(str(root), ["bb_p1", "bb_p2", "bb_dep"], "espidf")
            source = render_source(topo_sort(entries))
            # A single flag for token x -- not one per provider.
            self.assertEqual(source.count("static bool bb_app_avail_x = false;"), 1)
            # BOTH providers set it (OR semantics), each independently, on
            # their own success -- never reset to false by the other.
            self.assertIn(
                "    bb_app_rc = bb_p1_init();\n"
                "    if (bb_app_rc != BB_OK && bb_app_first_err == BB_OK) "
                "{ bb_app_first_err = bb_app_rc; }\n"
                "    if (bb_app_rc == BB_OK) {\n"
                "        bb_app_avail_x = true;\n"
                "    }\n",
                source,
            )
            self.assertIn(
                "    bb_app_rc = bb_p2_init();\n"
                "    if (bb_app_rc != BB_OK && bb_app_first_err == BB_OK) "
                "{ bb_app_first_err = bb_app_rc; }\n"
                "    if (bb_app_rc == BB_OK) {\n"
                "        bb_app_avail_x = true;\n"
                "    }\n",
                source,
            )
            self.assertIn(
                "    if (bb_app_avail_x) {\n"
                "        bb_app_rc = bb_dep_init();\n",
                source,
            )

    def test_dependent_ordered_after_both_providers(self):
        """The dependent must never run interleaved between the two
        providers -- topo_sort orders it after both, so its guard checks a
        flag that both providers have already had a chance to set."""
        with tempfile.TemporaryDirectory() as tmp:
            root = _fixture_root_with_multi_provider(tmp)
            entries = collect_entries(str(root), ["bb_p1", "bb_p2", "bb_dep"], "espidf")
            ordered = topo_sort(entries)
            self.assertEqual(
                [e.fn for e in ordered],
                ["bb_p1_init", "bb_p2_init", "bb_dep_init"],
            )
            source = render_source(ordered)
            p1_pos = source.index("bb_p1_init()")
            p2_pos = source.index("bb_p2_init()")
            dep_pos = source.index("bb_dep_init()")
            self.assertLess(p1_pos, dep_pos)
            self.assertLess(p2_pos, dep_pos)


def _fixture_root_with_cross_tier(tmp: str) -> Path:
    """Provider in tier=early, dependent in tier=pre_http -- a `requires=`
    satisfied by an EARLIER tier (wire_graph.topo_sort adds no same-tier
    edge for this case, since tier ordering alone already guarantees it),
    proving the file-scope static availability flag correctly threads the
    tier boundary (bb_app_init_early() sets it, bb_app_init_rest() reads
    it) rather than only ever working within a single tier/function."""
    root = Path(tmp)
    _make_component(
        root, "bb_early_provider",
        "#pragma once\n"
        "// bbtool:init tier=early fn=bb_early_provider_init provides=cross_x\n"
        "bb_err_t bb_early_provider_init(void);\n",
    )
    _make_component(
        root, "bb_late_dep",
        "#pragma once\n"
        "// bbtool:init tier=pre_http fn=bb_late_dep_init requires=cross_x\n"
        "bb_err_t bb_late_dep_init(void);\n",
    )
    return root


class TestGateDependentsCrossTier(unittest.TestCase):
    """B1-853: a `requires=` satisfied by an earlier tier's `provides=` must
    still be gated at RUNTIME (the provider could still fail), even though
    wire_graph.topo_sort treats cross-tier satisfaction as edge-free (tier
    ordering alone is enough for static ordering purposes)."""

    def test_provider_sets_flag_in_early_fn_dependent_guarded_in_rest_fn(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = _fixture_root_with_cross_tier(tmp)
            entries = collect_entries(str(root), ["bb_early_provider", "bb_late_dep"], "espidf")
            source = render_source(topo_sort(entries))

            early_fn = source[source.index("bb_app_init_early(void)"):source.index("bb_app_init_rest(void)")]
            rest_fn = source[source.index("bb_app_init_rest(void)"):]

            self.assertIn(
                "    bb_app_rc = bb_early_provider_init();\n"
                "    if (bb_app_rc != BB_OK && bb_app_first_err == BB_OK) "
                "{ bb_app_first_err = bb_app_rc; }\n"
                "    if (bb_app_rc == BB_OK) {\n"
                "        bb_app_avail_cross_x = true;\n"
                "    }\n",
                early_fn,
            )
            self.assertNotIn("bb_late_dep_init", early_fn)

            self.assertIn(
                "    if (bb_app_avail_cross_x) {\n"
                "        bb_app_rc = bb_late_dep_init();\n"
                "        if (bb_app_rc != BB_OK && bb_app_first_err == BB_OK) "
                "{ bb_app_first_err = bb_app_rc; }\n"
                "    } else {\n"
                '        bb_log_w(BB_APP_INIT_TAG, "skipping bb_late_dep_init: '
                'required provider unavailable");\n'
                "    }\n",
                rest_fn,
            )
            # The flag is declared exactly once, at file scope -- before
            # either function -- so it's the SAME static persisting across
            # both, not a per-function local.
            self.assertEqual(source.count("static bool bb_app_avail_cross_x = false;"), 1)
            self.assertLess(
                source.index("static bool bb_app_avail_cross_x = false;"),
                source.index("bb_app_init_early(void)"),
            )


def _fixture_root_with_args(tmp: str) -> Path:
    """A parameterized init fn (`args=`), mirroring
    bb_wifi_prov_autoinit(assets, n, cb) -- plus a plain requires=/provides=
    pair so the args= path can be proven to reuse the SAME guard/avail
    wrappers as every other entry."""
    root = Path(tmp)
    _make_component(
        root, "bb_provider",
        "#pragma once\n"
        "// bbtool:init tier=early fn=bb_provider_init provides=x\n"
        "bb_err_t bb_provider_init(void);\n",
    )
    _make_component(
        root, "bb_wifi_prov",
        "#pragma once\n"
        "// bbtool:init tier=regular fn=bb_wifi_prov_autoinit requires=x "
        "args=bb_wifi_prov_default_form_get(),1,NULL\n"
        "bb_err_t bb_wifi_prov_autoinit(const void *assets, unsigned n, void *cb);\n",
        requires=["bb_provider"],
    )
    return root


class TestArgsEmission(unittest.TestCase):
    def test_args_call_renders_verbatim_argument_list(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = _fixture_root_with_args(tmp)
            entries = collect_entries(str(root), ["bb_provider", "bb_wifi_prov"], "espidf")
            source = render_source(topo_sort(entries))
            self.assertIn(
                "bb_app_rc = bb_wifi_prov_autoinit(bb_wifi_prov_default_form_get(),1,NULL);",
                source,
            )

    def test_args_call_still_wrapped_in_requires_guard(self):
        """args= reuses _guard_requires unchanged -- a requires= entry with
        args= set is gated exactly like a zero-arg requires= entry."""
        with tempfile.TemporaryDirectory() as tmp:
            root = _fixture_root_with_args(tmp)
            entries = collect_entries(str(root), ["bb_provider", "bb_wifi_prov"], "espidf")
            source = render_source(topo_sort(entries))
            self.assertIn(
                "    if (bb_app_avail_x) {\n"
                "        bb_app_rc = bb_wifi_prov_autoinit("
                "bb_wifi_prov_default_form_get(),1,NULL);\n",
                source,
            )
            self.assertIn(
                '        bb_log_w(BB_APP_INIT_TAG, "skipping bb_wifi_prov_autoinit: '
                'required provider unavailable");\n',
                source,
            )

    def test_args_call_provides_avail_wrapper_reused(self):
        """args= reuses _emit_provides_avail unchanged -- an args= entry
        that ALSO provides= a token some other entry requires= still marks
        that token available on success, exactly like a zero-arg entry."""
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            _make_component(
                root, "bb_args_provider",
                "#pragma once\n"
                "// bbtool:init tier=early fn=bb_args_provider_init "
                "provides=y args=1,2\n"
                "bb_err_t bb_args_provider_init(int a, int b);\n",
            )
            _make_component(
                root, "bb_dep",
                "#pragma once\n"
                "// bbtool:init tier=early fn=bb_dep_init requires=y\n"
                "bb_err_t bb_dep_init(void);\n",
            )
            entries = collect_entries(str(root), ["bb_args_provider", "bb_dep"], "espidf")
            source = render_source(topo_sort(entries))
            self.assertIn(
                "    bb_app_rc = bb_args_provider_init(1,2);\n"
                "    if (bb_app_rc != BB_OK && bb_app_first_err == BB_OK) "
                "{ bb_app_first_err = bb_app_rc; }\n"
                "    if (bb_app_rc == BB_OK) {\n"
                "        bb_app_avail_y = true;\n"
                "    }\n",
                source,
            )


class TestConsumerManifestEntries(unittest.TestCase):
    """B1-731: a manifest entry (e.g. examples/*/main/) merges into the SAME
    tier graph as component entries, including satisfying a component's
    provides= via a manifest requires= (and vice versa) -- collect_manifest_
    entries is a distinct code path from collect_entries/discovery, but
    wire_graph.topo_sort must not need to know an entry's origin."""

    def test_manifest_entry_requires_component_provided_key(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = _fixture_root(tmp)  # bb_log: provides=log_stream / requires=log_stream
            manifest_path = Path(tmp) / "main" / "smoke_app.c"
            _write(
                manifest_path,
                "// bbtool:init tier=early fn=smoke_app_start requires=log_stream\n",
            )
            component_entries = collect_entries(str(root), ["bb_log"], "espidf")
            manifest_entries, manifest_provides = collect_manifest_entries(
                str(manifest_path), src_file="main/smoke_app.c")
            self.assertEqual([e.fn for e in manifest_entries], ["smoke_app_start"])
            self.assertEqual(manifest_provides, [])

            merged = component_entries + manifest_entries
            ordered = topo_sort(merged)
            # smoke_app_start requires= a key only bb_log_stream_init
            # provides= -- it must sort AFTER it despite being parsed from a
            # wholly separate "root" (a manifest file, never a component).
            self.assertEqual(
                [e.fn for e in ordered],
                ["bb_log_stream_init", "bb_log_config_init", "smoke_app_start"],
            )

    def test_manifest_requires_key_only_a_component_provides_without_manifest_hard_fails(self):
        """Negative companion: dropping the component leaves the manifest's
        requires= unsatisfied -- still a hard MissingProviderError, exactly
        as it would be for two components (proves the merge doesn't quietly
        relax requires= validation for manifest-origin entries)."""
        with tempfile.TemporaryDirectory() as tmp:
            manifest_path = Path(tmp) / "main" / "smoke_app.c"
            _write(
                manifest_path,
                "// bbtool:init tier=early fn=smoke_app_start requires=log_stream\n",
            )
            manifest_entries, _ = collect_manifest_entries(str(manifest_path))
            with self.assertRaises(MissingProviderError):
                topo_sort(manifest_entries)

    def test_manifest_provides_marker_collected_too(self):
        with tempfile.TemporaryDirectory() as tmp:
            manifest_path = Path(tmp) / "main" / "smoke_app.c"
            _write(
                manifest_path,
                "// bbtool:provides key=demo_sink symbol=bb_example_emit\n",
            )
            entries, provides = collect_manifest_entries(str(manifest_path))
            self.assertEqual(entries, [])
            self.assertEqual(len(provides), 1)
            self.assertEqual(provides[0].key, "demo_sink")

    def test_manifest_src_file_defaults_to_path_when_unspecified(self):
        with tempfile.TemporaryDirectory() as tmp:
            manifest_path = Path(tmp) / "main" / "smoke_app.c"
            _write(manifest_path, "// bbtool:init tier=early fn=smoke_app_start\n")
            entries, _ = collect_manifest_entries(str(manifest_path))
            self.assertEqual(entries[0].src_file, str(manifest_path))

    def test_manifest_provides_satisfies_component_consumes_setter_emitted(self):
        """The documented-but-previously-untested opposite direction: a
        manifest-declared `// bbtool:provides` satisfies a COMPONENT's
        `consumes=` -- the setter call must actually be emitted with the
        manifest-supplied symbol, not just silently accepted."""
        with tempfile.TemporaryDirectory() as tmp:
            # consumer only, no component provider -- the manifest is the
            # sole source of the demo_sink key.
            root = _fixture_root_with_consumes(tmp, provider=False, consumer=True)
            manifest_path = Path(tmp) / "main" / "smoke_app.c"
            _write(
                manifest_path,
                "// bbtool:provides key=demo_sink symbol=bb_manifest_emit\n",
            )
            component_entries = collect_entries(str(root), ["bb_example_consumer"], "espidf")
            component_provides = collect_provides_entries(str(root), ["bb_example_consumer"], "espidf")
            manifest_entries, manifest_provides = collect_manifest_entries(str(manifest_path))

            merged_entries = component_entries + manifest_entries
            merged_provides = list(component_provides) + manifest_provides
            source = render_source(topo_sort(merged_entries), merged_provides)
            self.assertIn("bb_example_set_emit(bb_manifest_emit, NULL);", source)

    def test_manifest_component_field_parses_through_collect_manifest_entries(self):
        """B1-1275: collect_manifest_entries places no restriction on
        component= -- the rejection lives in collect_entries only (see
        TestComponentFieldRejectedInComponentHeader above)."""
        with tempfile.TemporaryDirectory() as tmp:
            manifest_path = Path(tmp) / "main" / "smoke_app.c"
            _write(
                manifest_path,
                "// bbtool:init tier=regular fn=bb_wifi_prov_autoinit "
                "component=bb_wifi_prov args=NULL,0,NULL\n",
            )
            entries, _ = collect_manifest_entries(str(manifest_path))
            self.assertEqual(entries[0].component, "bb_wifi_prov")

    def test_manifest_out_field_parses_through_collect_manifest_entries(self):
        """out= places no restriction on collect_manifest_entries -- the
        rejection lives in collect_entries only (see
        TestOutFieldRejectedInComponentHeader above)."""
        with tempfile.TemporaryDirectory() as tmp:
            manifest_path = Path(tmp) / "main" / "smoke_app.c"
            _write(
                manifest_path,
                "// bbtool:init tier=regular fn=bb_lifecycle_register "
                "out=s_binding:bb_lifecycle_t args=cfg,&s_binding\n",
            )
            entries, _ = collect_manifest_entries(str(manifest_path))
            self.assertEqual(entries[0].out_var, "s_binding")
            self.assertEqual(entries[0].out_type, "bb_lifecycle_t")


class TestOutEmission(unittest.TestCase):
    """B1-1275-adjacent: out=<varname>:<c-type> emits a file-scope static
    declaration in the preamble; the call itself is unaffected (args= alone
    still renders the &varname reference the marker author wrote)."""

    def test_out_declaration_present_call_unchanged(self):
        with tempfile.TemporaryDirectory() as tmp:
            manifest_path = Path(tmp) / "main" / "smoke_app.c"
            _write(
                manifest_path,
                "// bbtool:init tier=regular fn=bb_lifecycle_register "
                "out=s_binding:bb_lifecycle_t args=cfg,&s_binding\n",
            )
            entries, _ = collect_manifest_entries(str(manifest_path))
            source = render_source(topo_sort(entries))
            self.assertIn("static bb_lifecycle_t s_binding;", source)
            self.assertIn(
                "bb_app_rc = bb_lifecycle_register(cfg,&s_binding);", source
            )

    def test_out_declaration_precedes_init_functions(self):
        with tempfile.TemporaryDirectory() as tmp:
            manifest_path = Path(tmp) / "main" / "smoke_app.c"
            _write(
                manifest_path,
                "// bbtool:init tier=regular fn=bb_lifecycle_register "
                "out=s_binding:bb_lifecycle_t args=cfg,&s_binding\n",
            )
            entries, _ = collect_manifest_entries(str(manifest_path))
            source = render_source(topo_sort(entries))
            decl_pos = source.index("static bb_lifecycle_t s_binding;")
            early_pos = source.index("bb_err_t bb_app_init_early(void)")
            self.assertLess(decl_pos, early_pos)

    def test_no_out_entries_omits_declaration_block_entirely(self):
        """Byte-stability guard: with no out= markers present, the
        declarations block must be entirely ABSENT, not empty-but-present."""
        with tempfile.TemporaryDirectory() as tmp:
            root = _fixture_root(tmp)
            entries = collect_entries(str(root), ["bb_log"], "espidf")
            source = render_source(topo_sort(entries))
            self.assertNotIn("out-parameter handles", source)
            self.assertNotIn("static bb_", source)

    def test_duplicate_out_varname_across_entries_raises_naming_both(self):
        """out= is manifest-only (see TestOutFieldRejectedInComponentHeader),
        so the duplicate-varname collision is exercised across two manifest
        entries rather than two component headers."""
        with tempfile.TemporaryDirectory() as tmp:
            manifest_a = Path(tmp) / "main" / "a.c"
            manifest_b = Path(tmp) / "main" / "b.c"
            _write(
                manifest_a,
                "// bbtool:init tier=early fn=bb_out_a_init out=s_shared:int\n",
            )
            _write(
                manifest_b,
                "// bbtool:init tier=early fn=bb_out_b_init out=s_shared:int\n",
            )
            entries_a, _ = collect_manifest_entries(str(manifest_a))
            entries_b, _ = collect_manifest_entries(str(manifest_b))
            with self.assertRaises(WireError) as ctx:
                render_source(topo_sort(entries_a + entries_b))
            msg = str(ctx.exception)
            self.assertIn("bb_out_a_init", msg)
            self.assertIn("bb_out_b_init", msg)
            self.assertIn("s_shared", msg)

    def test_out_varname_reserved_prefix_is_hard_error(self):
        with tempfile.TemporaryDirectory() as tmp:
            manifest_path = Path(tmp) / "main" / "smoke_app.c"
            _write(
                manifest_path,
                "// bbtool:init tier=early fn=bb_x_init out=bb_app_whatever:int\n",
            )
            entries, _ = collect_manifest_entries(str(manifest_path))
            with self.assertRaises(WireError) as ctx:
                render_source(topo_sort(entries))
            msg = str(ctx.exception)
            self.assertIn("bb_app_whatever", msg)
            self.assertIn("bb_x_init", msg)
            self.assertIn("reserved", msg)

    def test_out_varname_colliding_with_http_handle_is_hard_error(self):
        """Real failure shape #1: out=bb_app_http_handle would shadow the
        __auto_type server-handle capture local -- generated code compiles
        cleanly (C allows inner-scope shadowing) but any args= taking
        &bb_app_http_handle silently captures the address of the
        never-initialized file-scope static instead of the real handle."""
        with tempfile.TemporaryDirectory() as tmp:
            root = _fixture_root_with_http(tmp)
            manifest_path = Path(tmp) / "main" / "smoke_app.c"
            _write(
                manifest_path,
                "// bbtool:init tier=early fn=bb_x_init "
                "out=bb_app_http_handle:bb_http_handle_t\n",
            )
            component_entries = collect_entries(
                str(root), ["bb_log", "bb_http", "bb_routes"], "espidf")
            manifest_entries, _ = collect_manifest_entries(str(manifest_path))
            with self.assertRaises(WireError) as ctx:
                render_source(topo_sort(component_entries + manifest_entries))
            self.assertIn("bb_app_http_handle", str(ctx.exception))

    def test_out_varname_colliding_with_avail_flag_is_hard_error(self):
        """Real failure shape #2: on a composition that actually has a
        requires=/provides= guarded token, out=bb_app_avail_<token> would
        collide with the generated `static bool bb_app_avail_<token>;` flag
        -- a duplicate-definition compile error, exactly the class the
        duplicate-varname guard's own comment says it exists to prevent."""
        with tempfile.TemporaryDirectory() as tmp:
            root = _fixture_root(tmp)  # bb_log: provides=log_stream / requires=log_stream
            manifest_path = Path(tmp) / "main" / "smoke_app.c"
            _write(
                manifest_path,
                "// bbtool:init tier=early fn=bb_x_init "
                "out=bb_app_avail_log_stream:int\n",
            )
            component_entries = collect_entries(str(root), ["bb_log"], "espidf")
            manifest_entries, _ = collect_manifest_entries(str(manifest_path))
            with self.assertRaises(WireError) as ctx:
                render_source(topo_sort(component_entries + manifest_entries))
            self.assertIn("bb_app_avail_log_stream", str(ctx.exception))

    def test_out_varname_colliding_with_init_tag_macro_is_hard_error(self):
        """BB_APP_INIT_TAG is uppercase (a macro, not a bb_app_-prefixed
        variable) -- it needs its own explicit reservation alongside the
        prefix rule, not just the lowercase 'bb_app_' prefix check."""
        with tempfile.TemporaryDirectory() as tmp:
            root = _fixture_root(tmp)
            manifest_path = Path(tmp) / "main" / "smoke_app.c"
            _write(
                manifest_path,
                "// bbtool:init tier=early fn=bb_x_init out=BB_APP_INIT_TAG:int\n",
            )
            component_entries = collect_entries(str(root), ["bb_log"], "espidf")
            manifest_entries, _ = collect_manifest_entries(str(manifest_path))
            with self.assertRaises(WireError) as ctx:
                render_source(topo_sort(component_entries + manifest_entries))
            self.assertIn("BB_APP_INIT_TAG", str(ctx.exception))

    def test_out_varname_near_miss_case_is_accepted(self):
        """Pins a DELIBERATE decision, not incidental coverage: the reserved
        check is startswith("bb_app_") -- case-sensitive -- because C
        identifiers are case-sensitive, so an uppercase/mixed-case near-miss
        (e.g. BB_APP_FOO, Bb_App_x) cannot actually collide with any
        identifier codegen emits (all lowercase bb_app_*, plus the one exact
        BB_APP_INIT_TAG reservation). _RESERVED_OUT_VAR_EXACT deliberately
        holds ONLY that one macro name, not a general uppercase rule.

        DO NOT delete this test as "just a near-miss, not a real case": a
        future reader "hardening" the check by casefolding the comparison
        would silently start rejecting valid author-chosen varnames, and
        nothing else in this suite would catch that regression -- every
        other out= test either uses an unambiguously-safe name or one of the
        actually-reserved ones."""
        with tempfile.TemporaryDirectory() as tmp:
            manifest_path = Path(tmp) / "main" / "smoke_app.c"
            _write(
                manifest_path,
                "// bbtool:init tier=early fn=bb_x_init out=BB_APP_FOO:int\n"
                "// bbtool:init tier=early fn=bb_y_init out=Bb_App_x:int\n",
            )
            entries, _ = collect_manifest_entries(str(manifest_path))
            source = render_source(topo_sort(entries))
            self.assertIn("static int BB_APP_FOO;", source)
            self.assertIn("static int Bb_App_x;", source)


class TestByteStabilityNoOutMarkers(unittest.TestCase):
    """The most important test in this commit: existing http_server/args=/
    consumes= fixtures (none of which use out=) must render BYTE-IDENTICAL
    output before and after out= support exists -- the declarations block
    must be entirely absent, never empty-but-present."""

    def test_http_fixture_unaffected_by_out_support(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = _fixture_root_with_http(tmp)
            entries = collect_entries(str(root), ["bb_log", "bb_http", "bb_routes"], "espidf")
            source = render_source(topo_sort(entries))
            self.assertNotIn("out-parameter handles", source)

    def test_args_fixture_unaffected_by_out_support(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = _fixture_root_with_args(tmp)
            entries = collect_entries(str(root), ["bb_provider", "bb_wifi_prov"], "espidf")
            source = render_source(topo_sort(entries))
            self.assertNotIn("out-parameter handles", source)


class TestWildcardLastGuard(unittest.TestCase):
    """B1-1280: provides=http_wildcard_last guard -- a server=true entry
    (codegen's only route-registration signal) sorting at or after the
    marked wildcard-catching entry in the final topo-sorted list is a hard
    WireError, naming both sites."""

    def test_server_entry_sorting_after_wildcard_raises_naming_both_sites(self):
        """Explicit order= on BOTH entries, route order= places it after the
        wildcard's order= -- the general (not no-order-specific) violation."""
        with tempfile.TemporaryDirectory() as tmp:
            root = _fixture_root_with_wildcard(tmp, route_order=2, wildcard_order=1)
            entries = collect_entries(str(root), ["bb_log", "bb_http", "bb_routes", "bb_wildcard"], "espidf")
            ordered = topo_sort(entries)
            with self.assertRaises(WireError) as ctx:
                render_source(ordered)
            message = str(ctx.exception)
            self.assertIn("fn=bb_routes_register", message)
            self.assertIn("fn=bb_wildcard_register", message)
            self.assertIn("bb_routes.h", message)
            self.assertIn("bb_wildcard.h", message)

    def test_correct_composition_route_before_wildcard_does_not_fire(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = _fixture_root_with_wildcard(tmp, route_order=1, wildcard_order=2)
            entries = collect_entries(str(root), ["bb_log", "bb_http", "bb_routes", "bb_wildcard"], "espidf")
            ordered = topo_sort(entries)
            source = render_source(ordered)  # must not raise
            self.assertIn("bb_routes_register(bb_app_http_handle);", source)
            self.assertIn("bb_wildcard_register();", source)

    def test_no_order_route_after_explicit_order_wildcard_regression(self):
        """B1-1278 regression, named per ticket: an entry with NO order=
        (sorts to +inf, wire_graph.py's `_tie_key`) added after a marked
        entry WITH an explicit order= must still be caught -- an explicit
        order= on the wildcard entry alone does NOT self-protect."""
        with tempfile.TemporaryDirectory() as tmp:
            root = _fixture_root_with_wildcard(tmp, route_order=None, wildcard_order=1)
            entries = collect_entries(str(root), ["bb_log", "bb_http", "bb_routes", "bb_wildcard"], "espidf")
            ordered = topo_sort(entries)
            with self.assertRaises(WireError) as ctx:
                render_source(ordered)
            message = str(ctx.exception)
            self.assertIn("fn=bb_routes_register", message)
            self.assertIn("fn=bb_wildcard_register", message)

    def test_duplicate_wildcard_last_provider_raises(self):
        """Two entries both marking provides=http_wildcard_last must be a
        hard WireError -- mirrors the http_server singleton check: ambiguous
        which wildcard registration the guard is meant to protect."""
        with tempfile.TemporaryDirectory() as tmp:
            root = _fixture_root_with_wildcard(tmp, route_order=1, wildcard_order=2)
            _make_component(
                root, "bb_wildcard2",
                "#pragma once\n"
                "// bbtool:init tier=regular fn=bb_wildcard2_register "
                "provides=http_wildcard_last order=3\n"
                "bb_err_t bb_wildcard2_register(void);\n",
            )
            entries = collect_entries(
                str(root), ["bb_log", "bb_http", "bb_routes", "bb_wildcard", "bb_wildcard2"], "espidf")
            ordered = topo_sort(entries)
            with self.assertRaises(WireError) as ctx:
                render_source(ordered)
            message = str(ctx.exception)
            self.assertIn("bb_wildcard_register", message)
            self.assertIn("bb_wildcard2_register", message)
            self.assertIn("http_wildcard_last", message)

    def test_real_composition_shape_manifest_args_entry_passes(self):
        """Real-wiring-shaped regression: mirrors examples/smoke/main/bb_wire.h's
        actual bb_wifi_prov_autoinit marker -- a MANIFEST entry (collect_
        manifest_entries, never collect_entries) carrying BOTH `args=` (not
        `server=true` -- the prov entry threads the http handle internally,
        never as a codegen-supplied argument) and `provides=http_wildcard_last`,
        with NO explicit order=, merged against component `server=true` route
        entries exactly as commands.codegen.run merges them. Must NOT raise:
        proves the guard's singleton/ordering check works over the real
        merge-then-topo_sort shape, not only a synthetic single-collector
        fixture."""
        with tempfile.TemporaryDirectory() as tmp:
            root = _fixture_root_with_http(tmp)  # bb_http (http_server) + bb_routes (server=true)
            manifest_path = Path(tmp) / "main" / "bb_wire.h"
            _write(
                manifest_path,
                "// bbtool:init tier=regular fn=bb_wifi_prov_autoinit "
                "args=bb_wifi_prov_default_form_get(),1,NULL "
                "provides=http_wildcard_last\n",
            )
            component_entries = collect_entries(str(root), ["bb_log", "bb_http", "bb_routes"], "espidf")
            manifest_entries, _ = collect_manifest_entries(
                str(manifest_path), src_file="main/bb_wire.h")
            merged = component_entries + manifest_entries
            ordered = topo_sort(merged)
            source = render_source(ordered)  # must not raise
            self.assertIn(
                "bb_wifi_prov_autoinit(bb_wifi_prov_default_form_get(),1,NULL);", source)
            prov_pos = source.index("bb_wifi_prov_autoinit(")
            route_pos = source.index("bb_routes_register(bb_app_http_handle);")
            self.assertLess(route_pos, prov_pos)

    def test_real_composition_shape_second_manifest_route_after_prov_raises(self):
        """B1-1278 regression, real-wiring shape: `commands.codegen.run`
        merges as `entries = collect_entries(...) + manifest_entries`
        (components ALWAYS precede manifest entries in the merge), so a
        component-header route can never structurally land after a manifest
        entry -- the real hazard this key closes is a SECOND manifest marker
        (a `server=true` route -- e.g. a `/ping` handwired into the SAME
        manifest file) textually added AFTER the wildcard-marked entry with
        no order=, which is exactly what PR #1140 did: it added the prov
        marker to bb_wire.h without accounting for a route-registering entry
        that ends up composed after it. `collect_manifest_entries` preserves
        file (parse) order, so this fixture reproduces that shape precisely."""
        with tempfile.TemporaryDirectory() as tmp:
            root = _fixture_root_with_http(tmp)
            manifest_path = Path(tmp) / "main" / "bb_wire.h"
            _write(
                manifest_path,
                "// bbtool:init tier=regular fn=bb_wifi_prov_autoinit "
                "args=bb_wifi_prov_default_form_get(),1,NULL "
                "provides=http_wildcard_last\n"
                "// bbtool:init tier=regular fn=bb_ping_route_register server=true\n",
            )
            component_entries = collect_entries(str(root), ["bb_log", "bb_http", "bb_routes"], "espidf")
            manifest_entries, _ = collect_manifest_entries(
                str(manifest_path), src_file="main/bb_wire.h")
            merged = component_entries + manifest_entries
            ordered = topo_sort(merged)
            with self.assertRaises(WireError) as ctx:
                render_source(ordered)
            message = str(ctx.exception)
            self.assertIn("fn=bb_ping_route_register", message)
            self.assertIn("fn=bb_wifi_prov_autoinit", message)

    def test_no_wildcard_marker_present_no_guard_effect(self):
        """A composition that never uses provides=http_wildcard_last is
        entirely unaffected by this guard -- the singleton/ordering checks
        are both skipped when wildcard_providers is empty."""
        with tempfile.TemporaryDirectory() as tmp:
            root = _fixture_root_with_http(tmp)
            entries = collect_entries(str(root), ["bb_log", "bb_http", "bb_routes"], "espidf")
            source = render_source(topo_sort(entries))  # must not raise
            self.assertNotIn("http_wildcard_last", source)

    def test_registers_routes_args_entry_sorting_after_wildcard_raises(self):
        """B1-1280 blind-spot closure: an `args=`-shaped entry (structurally
        unable to carry server=true, the guard's original only signal)
        opting into the guard via registers_routes=true and sorting AFTER
        the wildcard entry must be a hard WireError, naming both sites --
        this is the exact regression this key exists to catch (it must fail
        without the registers_routes= support, since server=true alone would
        never see this entry at all)."""
        with tempfile.TemporaryDirectory() as tmp:
            root = _fixture_root_with_wildcard_args_route(
                tmp, route_order=2, wildcard_order=1, registers_routes=True)
            entries = collect_entries(
                str(root), ["bb_log", "bb_http", "bb_args_route", "bb_wildcard"], "espidf")
            ordered = topo_sort(entries)
            with self.assertRaises(WireError) as ctx:
                render_source(ordered)
            message = str(ctx.exception)
            self.assertIn("fn=bb_args_route_register", message)
            self.assertIn("fn=bb_wildcard_register", message)
            self.assertIn("registers_routes=true", message)

    def test_wildcard_entry_itself_does_not_self_flag(self):
        """The wildcard entry (args=-shaped, provides=http_wildcard_last) is
        excluded from the guard's scan by identity (`e is wildcard_entry`),
        not by the absence of registers_routes= -- proven by giving the
        wildcard entry BOTH provides=http_wildcard_last AND
        registers_routes=true (a realistic combination: the wildcard entry
        itself genuinely registers routes) and asserting it still renders
        without raising."""
        with tempfile.TemporaryDirectory() as tmp:
            root = _fixture_root(tmp)
            _make_component(
                root, "bb_wildcard",
                "#pragma once\n"
                "// bbtool:init tier=regular fn=bb_wildcard_register "
                "provides=http_wildcard_last registers_routes=true\n"
                "bb_err_t bb_wildcard_register(void);\n",
            )
            entries = collect_entries(str(root), ["bb_log", "bb_wildcard"], "espidf")
            ordered = topo_sort(entries)
            source = render_source(ordered)  # must not raise
            self.assertIn("bb_wildcard_register();", source)

    def test_args_entry_without_registers_routes_after_wildcard_unaffected(self):
        """A plain `args=`-shaped entry that does NOT opt into the guard
        (registers_routes not set) sorting after the wildcard entry is NOT
        flagged -- registers_routes=true is opt-in, not inferred from
        args=/bb_app_http_handle text."""
        with tempfile.TemporaryDirectory() as tmp:
            root = _fixture_root_with_wildcard_args_route(
                tmp, route_order=2, wildcard_order=1, registers_routes=False)
            entries = collect_entries(
                str(root), ["bb_log", "bb_http", "bb_args_route", "bb_wildcard"], "espidf")
            ordered = topo_sort(entries)
            source = render_source(ordered)  # must not raise
            self.assertIn("bb_args_route_register(bb_app_http_handle,\"/ping\");", source)
            self.assertIn("bb_wildcard_register();", source)


class TestWildcardLastByteStability(unittest.TestCase):
    """B1-1280: a composition that never uses provides=http_wildcard_last
    must render byte-identical output to before this guard existed -- the
    guard adds a WireError gate only, never new emitted text."""

    def test_http_fixture_unaffected_by_wildcard_guard(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = _fixture_root_with_http(tmp)
            entries = collect_entries(str(root), ["bb_log", "bb_http", "bb_routes"], "espidf")
            source = render_source(topo_sort(entries))
            self.assertIn("__auto_type bb_app_http_handle = bb_http_start();", source)
            self.assertIn("bb_routes_register(bb_app_http_handle);", source)
            self.assertNotIn("wildcard", source)

    def test_args_fixture_unaffected_by_wildcard_guard(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = _fixture_root_with_args(tmp)
            entries = collect_entries(str(root), ["bb_provider", "bb_wifi_prov"], "espidf")
            source = render_source(topo_sort(entries))
            self.assertNotIn("wildcard", source)


if __name__ == "__main__":
    unittest.main()
