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


if __name__ == "__main__":
    unittest.main()
