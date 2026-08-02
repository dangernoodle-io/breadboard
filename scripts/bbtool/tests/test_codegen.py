"""codegen command tests: `bbtool codegen` resolves the composition ONCE and
emits BOTH artifacts (the COMPONENTS link-set .cmake fragment, and
bb_app_init.c + its sibling .cmake) from that one resolution. Fixture style
mirrors test_wire.py/test_composition.py (synthetic component trees, never
the real breadboard tree). `--components` is required (no `--composition`
preset shortcut)."""
import argparse
import contextlib
import io
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "commands"))

from commands.codegen import pio_main, run


def _write(path: Path, content: str = "") -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")


def _make_component(root: Path, name: str, header_body: str, requires=None, src: str = None) -> None:
    body = "idf_component_register(\n"
    if requires:
        body += f"    REQUIRES {' '.join(requires)}\n"
    body += ")\n"
    comp = root / "components" / name
    _write(comp / "CMakeLists.txt", body)
    _write(comp / "include" / f"{name}.h", header_body)
    if src is not None:
        _write(comp / "src" / f"{name}.c", src)


def _fixture_root(tmp: str) -> Path:
    """bb_log-alike: stream then config (requires=log_stream) -- the same
    order the hand-wired examples/floor/main/floor_app.c calls
    bb_log_stream_init() then bb_log_config_init() -- plus an independent
    bb_meminfo with no markers at all (no init function)."""
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


class TestRunCli(unittest.TestCase):
    def test_run_emits_both_artifacts_from_one_resolution(self):
        """A single `codegen` invocation writes BOTH the link-set fragment
        AND bb_app_init.c (+ its .cmake sibling) -- the fold's core claim."""
        with tempfile.TemporaryDirectory() as tmp:
            root = _fixture_root(tmp)
            components_out = str(root / "out" / "bb_autowire_components.cmake")
            wire_out = str(root / "out" / "bb_app_init.c")
            args = argparse.Namespace(
                root=str(root), components="bb_log,bb_meminfo",
                platform="espidf", components_out=components_out, wire_out=wire_out,
            )
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                rc = run(args)
            self.assertEqual(rc, 0)

            # Link-set fragment (same content `bbtool autowire` used to write).
            self.assertTrue(os.path.isfile(components_out))
            components_content = Path(components_out).read_text(encoding="utf-8")
            self.assertIn("set(BB_AUTOWIRE_REQUIRES bb_log bb_meminfo)", components_content)
            self.assertIn("set(BB_AUTOWIRE_COMPONENTS main bb_log bb_meminfo)", components_content)

            # bb_app_init.c + sibling .cmake (same content `bbtool wire` used to write).
            self.assertTrue(os.path.isfile(wire_out))
            wire_cmake_out = str(root / "out" / "bb_app_init.cmake")
            self.assertTrue(os.path.isfile(wire_cmake_out))
            self.assertIn("BB_WIRE_GENERATED_SOURCE", Path(wire_cmake_out).read_text())

            # Floor-validation carry-over: generated init order must match the
            # hand-wired examples/floor/main/floor_app.c sequence -- stream
            # init before config init.
            source = Path(wire_out).read_text(encoding="utf-8")
            stream_pos = source.index("bb_log_stream_init()")
            config_pos = source.index("bb_log_config_init()")
            self.assertLess(stream_pos, config_pos)
            self.assertIn("bb_err_t bb_app_init(void)", source)

    def test_run_requires_components(self):
        args = argparse.Namespace(root=os.getcwd(), components=None,
                                   platform="espidf", components_out=None, wire_out=None)
        rc = run(args)
        self.assertEqual(rc, 1)

    def test_run_unknown_component_returns_error(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = _fixture_root(tmp)
            args = argparse.Namespace(root=str(root), components="bb_ghost",
                                       platform="espidf", components_out=None, wire_out=None)
            rc = run(args)
            self.assertEqual(rc, 1)

    def test_run_orders_storage_backend_register_before_its_consumer(self):
        """bb_storage_nvs/bb_wifi-alike fixture (fix for the latent bug where
        the "nvs" bb_storage backend was registered only in a selftest, never
        on the real boot path): a provides=storage_nvs entry must be ordered
        before a requires=storage_nvs consumer in the same EARLY tier, even
        though there is no CMake REQUIRES between the two components and
        nothing else pins their relative order.

        Fixture names are deliberately chosen so the CONSUMER
        (bb_aaa_consumer) alphabetically PRECEDES the PROVIDER
        (bb_zzz_provider) -- sorted/parse order alone would place the
        consumer first, so only the requires=/provides= edge can force the
        provider ahead of it. This makes the ordering assertion load-bearing
        rather than a byproduct of alphabetical parse-order coincidence."""
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            _make_component(
                root, "bb_aaa_consumer",
                "#pragma once\n"
                "// bbtool:init tier=early fn=bb_aaa_consumer_init requires=zzz_backend\n"
                "bb_err_t bb_aaa_consumer_init(void);\n",
            )
            _make_component(
                root, "bb_zzz_provider",
                "#pragma once\n"
                "// bbtool:init tier=early fn=bb_zzz_provider_register provides=zzz_backend\n"
                "bb_err_t bb_zzz_provider_register(void);\n",
            )
            components_out = str(root / "out" / "bb_autowire_components.cmake")
            wire_out = str(root / "out" / "bb_app_init.c")
            args = argparse.Namespace(
                root=str(root), components="bb_aaa_consumer,bb_zzz_provider",
                platform="espidf", components_out=components_out, wire_out=wire_out,
            )
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                rc = run(args)
            self.assertEqual(rc, 0)

            source = Path(wire_out).read_text(encoding="utf-8")
            register_pos = source.index("bb_zzz_provider_register()")
            consumer_pos = source.index("bb_aaa_consumer_init()")
            self.assertLess(register_pos, consumer_pos)

    def test_run_missing_provider_returns_error(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            _make_component(
                root, "bb_bad",
                "#pragma once\n// bbtool:init tier=early fn=bb_bad_init requires=ghost\n"
                "bb_err_t bb_bad_init(void);\n",
            )
            args = argparse.Namespace(root=str(root), components="bb_bad",
                                       platform="espidf", components_out=None, wire_out=None)
            rc = run(args)
            self.assertEqual(rc, 1)

    def test_run_uses_default_output_paths_when_unspecified(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = _fixture_root(tmp)
            args = argparse.Namespace(
                root=str(root), components="bb_log,bb_meminfo",
                platform="espidf", components_out=None, wire_out=None,
            )
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                rc = run(args)
            self.assertEqual(rc, 0)
            self.assertTrue(os.path.isfile(
                root / "examples" / "smoke" / "main" / "generated" / "bb_autowire_components.cmake"
            ))
            self.assertTrue(os.path.isfile(root / "main" / "generated" / "bb_app_init.c"))
            self.assertTrue(os.path.isfile(root / "main" / "generated" / "bb_app_init.cmake"))


class TestFormatRegistryBackendWarning(unittest.TestCase):
    """B1-985: `bbtool codegen` warns to stderr (non-fatal) when the resolved
    composition pulls a format-registry consumer with zero bb_serialize_*
    backends composed alongside it."""

    def _fixture_root(self, tmp: str, with_backend: bool) -> Path:
        root = Path(tmp)
        _make_component(root, "bb_serialize", "#pragma once\n")
        if with_backend:
            _make_component(
                root, "bb_serialize_json", "#pragma once\n", requires=["bb_serialize"],
                src=(
                    "#include \"bb_serialize_format.h\"\n"
                    "void bb_serialize_json_register_format(void) { "
                    "bb_serialize_format_register(0, 0); }\n"
                ),
            )
        _make_component(
            root, "bb_consumer", "#pragma once\nbb_err_t bb_consumer_get(void);\n",
            requires=["bb_serialize"],
            src=(
                "#include \"bb_serialize_format.h\"\n"
                "bb_err_t bb_consumer_get(void) { "
                "return bb_serialize_format_get_render(0) ? 0 : -1; }\n"
            ),
        )
        return root

    def test_consumer_without_backend_warns_on_stderr(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = self._fixture_root(tmp, with_backend=False)
            components_out = str(root / "out" / "bb_autowire_components.cmake")
            wire_out = str(root / "out" / "bb_app_init.c")
            args = argparse.Namespace(
                root=str(root), components="bb_consumer",
                platform="espidf", components_out=components_out, wire_out=wire_out,
            )
            out_buf, err_buf = io.StringIO(), io.StringIO()
            with contextlib.redirect_stdout(out_buf), contextlib.redirect_stderr(err_buf):
                rc = run(args)
            self.assertEqual(rc, 0)
            self.assertIn("bb_consumer", err_buf.getvalue())
            self.assertIn("bb_serialize_*", err_buf.getvalue())

    def test_consumer_with_backend_does_not_warn(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = self._fixture_root(tmp, with_backend=True)
            components_out = str(root / "out" / "bb_autowire_components.cmake")
            wire_out = str(root / "out" / "bb_app_init.c")
            args = argparse.Namespace(
                root=str(root), components="bb_consumer,bb_serialize_json",
                platform="espidf", components_out=components_out, wire_out=wire_out,
            )
            out_buf, err_buf = io.StringIO(), io.StringIO()
            with contextlib.redirect_stdout(out_buf), contextlib.redirect_stderr(err_buf):
                rc = run(args)
            self.assertEqual(rc, 0)
            self.assertEqual(err_buf.getvalue(), "")


def _write_toml(root: Path, text: str) -> None:
    (root / "bbtool.toml").write_text(text, encoding="utf-8")


class TestBoardFlag(unittest.TestCase):
    """B1-747: `--board` resolves the requested set from the manifest
    (`[capability.*]`/`[board.*]` in bbtool.toml) instead of `--components`
    -- exactly one of the two is required."""

    def test_components_and_board_together_rejected(self):
        args = argparse.Namespace(
            root=os.getcwd(), components="bb_log", board="native",
            platform="espidf", components_out=None, wire_out=None,
        )
        rc = run(args)
        self.assertEqual(rc, 1)

    def test_neither_components_nor_board_rejected(self):
        args = argparse.Namespace(
            root=os.getcwd(), components=None, board=None,
            platform="espidf", components_out=None, wire_out=None,
        )
        rc = run(args)
        self.assertEqual(rc, 1)

    def test_board_resolves_via_manifest(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = _fixture_root(tmp)
            _write_toml(root, (
                '[capability.demo]\n'
                'components = ["bb_log", "bb_meminfo"]\n\n'
                '[board.demo_board]\n'
                'capabilities = ["demo"]\n'
            ))
            components_out = str(root / "out" / "bb_autowire_components.cmake")
            wire_out = str(root / "out" / "bb_app_init.c")
            args = argparse.Namespace(
                root=str(root), components=None, board="demo_board",
                wire_board=None, platform="espidf",
                components_out=components_out, wire_out=wire_out,
            )
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                rc = run(args)
            self.assertEqual(rc, 0)
            content = Path(components_out).read_text(encoding="utf-8")
            self.assertIn('set(BB_AUTOWIRE_BOARD "demo_board")', content)
            self.assertIn("set(BB_AUTOWIRE_REQUIRES bb_log bb_meminfo)", content)

    def test_board_unknown_returns_error(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = _fixture_root(tmp)
            _write_toml(root, '[capability.demo]\ncomponents = ["bb_log"]\n')
            args = argparse.Namespace(
                root=str(root), components=None, board="ghost_board",
                wire_board=None, platform="espidf",
                components_out=None, wire_out=None,
            )
            rc = run(args)
            self.assertEqual(rc, 1)


class TestAuthoritativeClosure(unittest.TestCase):
    """B1-747's core claim: the resolved closure is now AUTHORITATIVE, not
    additive -- dropping a component from a board's manifest set actually
    drops its transitive-only deps too, while deps shared with another
    requested component survive.

    This is a MANIFEST-layer claim (which capability.components a board
    activates), not a CMake-REQUIRES-layer one (a component's own REQUIRES
    is fixed regardless of who requests it, so it can't model "C is only
    pulled in via A" -- that's exactly why smoke's board-conditional groups
    had to move to the manifest, not stay expressed as CMake deps). Fixture
    components carry no CMake REQUIRES; the graph lives entirely in two
    capabilities:
        capability "cap_a": comp_a, comp_b, comp_c  (A "brings" B and C)
        capability "cap_d": comp_d, comp_b          (D brings B only)
    board_ad activates both capabilities -> {a,b,c,d}. board_d activates
    only cap_d -> {b,d}, NOT c -- comp_c is only reachable via cap_a, which
    board_d never activates."""

    def _fixture_root(self, tmp: str) -> Path:
        root = Path(tmp)
        _make_component(root, "comp_a", "#pragma once\n")
        _make_component(root, "comp_b", "#pragma once\n")
        _make_component(root, "comp_c", "#pragma once\n")
        _make_component(root, "comp_d", "#pragma once\n")
        _write_toml(root, (
            '[capability.cap_a]\n'
            'components = ["comp_a", "comp_b", "comp_c"]\n\n'
            '[capability.cap_d]\n'
            'components = ["comp_d", "comp_b"]\n\n'
            '[board.board_ad]\n'
            'capabilities = ["cap_a", "cap_d"]\n\n'
            '[board.board_d]\n'
            'capabilities = ["cap_d"]\n'
        ))
        return root

    def _resolved_requires(self, root: Path, board: str) -> set:
        components_out = str(root / f"out-{board}" / "bb_autowire_components.cmake")
        wire_out = str(root / f"out-{board}" / "bb_app_init.c")
        args = argparse.Namespace(
            root=str(root), components=None, board=board, wire_board=None,
            platform="espidf", components_out=components_out, wire_out=wire_out,
        )
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rc = run(args)
        self.assertEqual(rc, 0)
        content = Path(components_out).read_text(encoding="utf-8")
        line = next(l for l in content.splitlines() if l.startswith("set(BB_AUTOWIRE_REQUIRES "))
        names = line[len("set(BB_AUTOWIRE_REQUIRES "):].rstrip(")").split()
        return set(names)

    def test_requesting_both_dependents_includes_shared_transitive_dep(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = self._fixture_root(tmp)
            resolved = self._resolved_requires(root, "board_ad")
            self.assertEqual(resolved, {"comp_a", "comp_b", "comp_c", "comp_d"})

    def test_dropping_one_dependent_drops_its_transitive_only_dep(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = self._fixture_root(tmp)
            resolved = self._resolved_requires(root, "board_d")
            self.assertEqual(resolved, {"comp_b", "comp_d"})
            self.assertNotIn("comp_c", resolved)


class TestMultiRootDiscovery(unittest.TestCase):
    """B1-1084: `--extra-root` (CLI, repeatable) and `[discovery].extra_roots`
    (bbtool.toml, resolved relative to the toml's own dir) both thread into
    codegen's discovery root list -- a component that exists ONLY under a
    non-primary root resolves and wires correctly end-to-end. Also covers
    the concrete `discovery.CollisionError` fix: an uncaught collision must
    produce a clean `bbtool codegen: error: ...` stderr line, never a raw
    traceback."""

    def _extra_root_fixture(self, tmp: str):
        root = Path(tmp) / "consumer"
        extra = Path(tmp) / "extra"
        _write(root / ".keep", "")  # empty consumer root -- no components/ of its own
        _make_component(
            extra, "bb_ext",
            "#pragma once\n"
            "// bbtool:init tier=early fn=bb_ext_init\n"
            "bb_err_t bb_ext_init(void);\n",
        )
        return root, extra

    def test_extra_root_cli_flag_resolves_component(self):
        with tempfile.TemporaryDirectory() as tmp:
            root, extra = self._extra_root_fixture(tmp)
            components_out = str(root / "out" / "bb_autowire_components.cmake")
            wire_out = str(root / "out" / "bb_app_init.c")
            args = argparse.Namespace(
                root=str(root), components="bb_ext", platform="espidf",
                components_out=components_out, wire_out=wire_out,
                extra_root=[str(extra)],
            )
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                rc = run(args)
            self.assertEqual(rc, 0)
            source = Path(wire_out).read_text(encoding="utf-8")
            self.assertIn("bb_ext_init()", source)
            # Fork 2: a non-primary-root marker's src_file is absolute --
            # surfaces in the codegen stdout entry listing.
            self.assertIn(str(extra), buf.getvalue())

    def test_extra_root_toml_config_resolves_component(self):
        with tempfile.TemporaryDirectory() as tmp:
            root, extra = self._extra_root_fixture(tmp)
            _write_toml(root, f'[discovery]\nextra_roots = ["{extra.as_posix()}"]\n')
            components_out = str(root / "out" / "bb_autowire_components.cmake")
            wire_out = str(root / "out" / "bb_app_init.c")
            args = argparse.Namespace(
                root=str(root), components="bb_ext", platform="espidf",
                components_out=components_out, wire_out=wire_out,
                config=str(root / "bbtool.toml"),
            )
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                rc = run(args)
            self.assertEqual(rc, 0)
            source = Path(wire_out).read_text(encoding="utf-8")
            self.assertIn("bb_ext_init()", source)

    def test_extra_root_toml_relative_path_resolves_against_config_dir(self):
        """[discovery].extra_roots entries resolve relative to the toml
        file's OWN dir (mirrors [plugins].paths / load_plugins), not the
        consumer --root or cwd."""
        with tempfile.TemporaryDirectory() as tmp:
            root, extra = self._extra_root_fixture(tmp)
            config_dir = Path(tmp) / "cfgdir"
            config_dir.mkdir()
            rel = os.path.relpath(str(extra), str(config_dir))
            _write_toml(config_dir, f'[discovery]\nextra_roots = ["{rel}"]\n')
            components_out = str(root / "out" / "bb_autowire_components.cmake")
            wire_out = str(root / "out" / "bb_app_init.c")
            args = argparse.Namespace(
                root=str(root), components="bb_ext", platform="espidf",
                components_out=components_out, wire_out=wire_out,
                config=str(config_dir / "bbtool.toml"),
            )
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                rc = run(args)
            self.assertEqual(rc, 0)
            source = Path(wire_out).read_text(encoding="utf-8")
            self.assertIn("bb_ext_init()", source)

    def test_collision_across_root_and_extra_root_is_clean_error_not_traceback(self):
        """The CollisionError fix (codegen.py's except tuple): a name
        collision across --root/--extra-root must produce a clean
        'bbtool codegen: error: ...' stderr line (rc=1), never an uncaught
        traceback."""
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "consumer"
            extra = Path(tmp) / "extra"
            _make_component(root, "bb_dup", "#pragma once\n")
            _make_component(extra, "bb_dup", "#pragma once\n")
            args = argparse.Namespace(
                root=str(root), components="bb_dup", platform="espidf",
                components_out=None, wire_out=None, extra_root=[str(extra)],
            )
            err_buf = io.StringIO()
            with contextlib.redirect_stderr(err_buf):
                rc = run(args)
            self.assertEqual(rc, 1)
            self.assertIn("bbtool codegen: error:", err_buf.getvalue())
            self.assertIn("bb_dup", err_buf.getvalue())


class TestConsumerManifestFlag(unittest.TestCase):
    """B1-731: `--consumer-manifest <path>` parses ONE extra file for
    markers and merges it into the wire entry set before topo_sort -- a
    manifest entry's requires= can be satisfied by a component's provides=
    (the forcing case: an example's main/ requiring a component-provided
    key), and omitting the flag must be byte-identical to today."""

    def test_manifest_absent_output_unchanged(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = _fixture_root(tmp)
            wire_out = str(root / "out" / "bb_app_init.c")
            args = argparse.Namespace(
                root=str(root), components="bb_log,bb_meminfo", platform="espidf",
                components_out=str(root / "out" / "c.cmake"), wire_out=wire_out,
            )
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                rc = run(args)
            self.assertEqual(rc, 0)
            without_flag = Path(wire_out).read_text(encoding="utf-8")

            wire_out2 = str(root / "out2" / "bb_app_init.c")
            args2 = argparse.Namespace(
                root=str(root), components="bb_log,bb_meminfo", platform="espidf",
                components_out=str(root / "out2" / "c.cmake"), wire_out=wire_out2,
                consumer_manifest=None,
            )
            buf2 = io.StringIO()
            with contextlib.redirect_stdout(buf2):
                rc2 = run(args2)
            self.assertEqual(rc2, 0)
            with_none_flag = Path(wire_out2).read_text(encoding="utf-8")
            self.assertEqual(without_flag, with_none_flag)

    def test_manifest_entry_requires_component_provided_key(self):
        """B1-731 forcing case: a manifest entry (standing in for an
        example's main/) requires= a key only a composed COMPONENT
        provides= -- must resolve and sort after it, never a
        MissingProviderError."""
        with tempfile.TemporaryDirectory() as tmp:
            root = _fixture_root(tmp)
            manifest_path = root / "examples" / "smoke" / "main" / "smoke_app.c"
            manifest_path.parent.mkdir(parents=True)
            manifest_path.write_text(
                "// bbtool:init tier=early fn=smoke_app_start requires=log_stream\n",
                encoding="utf-8",
            )
            wire_out = str(root / "out" / "bb_app_init.c")
            args = argparse.Namespace(
                root=str(root), components="bb_log,bb_meminfo", platform="espidf",
                components_out=str(root / "out" / "c.cmake"), wire_out=wire_out,
                consumer_manifest=str(manifest_path),
            )
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                rc = run(args)
            self.assertEqual(rc, 0)
            source = Path(wire_out).read_text(encoding="utf-8")
            stream_pos = source.index("bb_log_stream_init()")
            smoke_pos = source.index("smoke_app_start()")
            self.assertLess(stream_pos, smoke_pos)

    def test_manifest_entry_never_joins_component_namespace(self):
        """The manifest path must stay distinct from component discovery --
        a manifest containing a requires= key nothing (component OR
        manifest) provides= must still hard-fail, proving the manifest
        entries were merged, not silently ignored / never validated."""
        with tempfile.TemporaryDirectory() as tmp:
            root = _fixture_root(tmp)
            manifest_path = root / "examples" / "smoke" / "main" / "smoke_app.c"
            manifest_path.parent.mkdir(parents=True)
            manifest_path.write_text(
                "// bbtool:init tier=early fn=smoke_app_start requires=ghost_key\n",
                encoding="utf-8",
            )
            args = argparse.Namespace(
                root=str(root), components="bb_log,bb_meminfo", platform="espidf",
                components_out=None, wire_out=None,
                consumer_manifest=str(manifest_path),
            )
            rc = run(args)
            self.assertEqual(rc, 1)

    def test_manifest_missing_file_is_clean_error_not_traceback(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = _fixture_root(tmp)
            args = argparse.Namespace(
                root=str(root), components="bb_log,bb_meminfo", platform="espidf",
                components_out=None, wire_out=None,
                consumer_manifest=str(root / "no_such_manifest.c"),
            )
            err_buf = io.StringIO()
            with contextlib.redirect_stderr(err_buf):
                rc = run(args)
            self.assertEqual(rc, 1)
            self.assertIn("bbtool codegen: error:", err_buf.getvalue())

    def test_pre_existing_path_oserror_not_swallowed_by_manifest_catch(self):
        """The OSError catch is scoped to the --consumer-manifest read only
        -- an OSError from a PRE-EXISTING path (e.g. collect_entries hitting
        an unreadable component header) must propagate as a raw exception,
        not get silently downgraded to this command's clean one-line
        manifest-missing-file error."""
        with tempfile.TemporaryDirectory() as tmp:
            root = _fixture_root(tmp)
            manifest_path = root / "examples" / "smoke" / "main" / "smoke_app.c"
            manifest_path.parent.mkdir(parents=True)
            manifest_path.write_text(
                "// bbtool:init tier=early fn=smoke_app_start requires=log_stream\n",
                encoding="utf-8",
            )
            args = argparse.Namespace(
                root=str(root), components="bb_log,bb_meminfo", platform="espidf",
                components_out=None, wire_out=None,
                consumer_manifest=str(manifest_path),
            )
            with mock.patch(
                "commands.codegen.collect_entries",
                side_effect=OSError("permission denied: bb_log.h"),
            ):
                with self.assertRaises(OSError):
                    run(args)


class TestManifestComponentField(unittest.TestCase):
    """B1-1275: a manifest '// bbtool:init' marker's optional 'component='
    field folds the named component into the SAME resolved composition
    closure --components/--board feed (boards.resolve_transitive), including
    its transitive REQUIRES/PRIV_REQUIRES -- so the hand-sync a consumer
    previously needed (add the component to --components/[capability.*]
    AND emit the manifest call) disappears."""

    def _fixture_with_target(self, tmp: str) -> Path:
        """bb_target REQUIRES bb_target_dep -- a transitive REQUIRES chain
        distinct from the bb_log/bb_meminfo fixture, so 'component=bb_target'
        pulling in bb_target_dep too is load-bearing, not a coincidence of
        an already-flat dependency graph."""
        root = _fixture_root(tmp)
        _make_component(
            root, "bb_target_dep", "#pragma once\nbb_err_t bb_target_dep_init(void);\n",
        )
        _make_component(
            root, "bb_target",
            "#pragma once\n"
            "bb_err_t bb_wifi_prov_autoinit(void);\n",
            requires=["bb_target_dep"],
        )
        return root

    def _requires_set(self, components_out: str) -> set:
        content = Path(components_out).read_text(encoding="utf-8")
        line = next(l for l in content.splitlines() if l.startswith("set(BB_AUTOWIRE_REQUIRES "))
        return set(line[len("set(BB_AUTOWIRE_REQUIRES "):].rstrip(")").split())

    def test_manifest_component_field_pulls_component_into_closure(self):
        """The named component enters the REQUIRES/components-fragment
        closure exactly as if it had been listed in --components (proof
        target #1)."""
        with tempfile.TemporaryDirectory() as tmp:
            root = self._fixture_with_target(tmp)
            manifest_path = root / "main" / "smoke_app.c"
            _write(
                manifest_path,
                "// bbtool:init tier=regular fn=bb_wifi_prov_autoinit "
                "component=bb_target args=NULL\n",
            )
            components_out = str(root / "out" / "c.cmake")
            wire_out = str(root / "out" / "bb_app_init.c")
            args = argparse.Namespace(
                root=str(root), components="bb_log,bb_meminfo", platform="espidf",
                components_out=components_out, wire_out=wire_out,
                consumer_manifest=str(manifest_path),
            )
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                rc = run(args)
            self.assertEqual(rc, 0)
            self.assertIn("bb_target", self._requires_set(components_out))

    def test_manifest_component_field_pulls_transitive_priv_requires_too(self):
        """Proof target #2: bb_target's own REQUIRES (bb_target_dep) arrives
        transitively -- 'component=' does the SAME closure walk
        [capability.*].components would, not a shallow single-name add."""
        with tempfile.TemporaryDirectory() as tmp:
            root = self._fixture_with_target(tmp)
            manifest_path = root / "main" / "smoke_app.c"
            _write(
                manifest_path,
                "// bbtool:init tier=regular fn=bb_wifi_prov_autoinit "
                "component=bb_target args=NULL\n",
            )
            components_out = str(root / "out" / "c.cmake")
            wire_out = str(root / "out" / "bb_app_init.c")
            args = argparse.Namespace(
                root=str(root), components="bb_log,bb_meminfo", platform="espidf",
                components_out=components_out, wire_out=wire_out,
                consumer_manifest=str(manifest_path),
            )
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                rc = run(args)
            self.assertEqual(rc, 0)
            resolved = self._requires_set(components_out)
            self.assertIn("bb_target", resolved)
            self.assertIn("bb_target_dep", resolved)

    def test_manifest_component_field_matches_explicit_components_list(self):
        """'component=bb_target' resolves to the IDENTICAL closure as
        listing bb_target directly in --components -- same resolver, same
        result, proving there's no separate/lesser resolution path."""
        with tempfile.TemporaryDirectory() as tmp:
            root = self._fixture_with_target(tmp)

            direct_out = str(root / "direct" / "c.cmake")
            direct_args = argparse.Namespace(
                root=str(root), components="bb_log,bb_meminfo,bb_target",
                platform="espidf", components_out=direct_out,
                wire_out=str(root / "direct" / "bb_app_init.c"),
            )
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                rc = run(direct_args)
            self.assertEqual(rc, 0)

            manifest_path = root / "main" / "smoke_app.c"
            _write(
                manifest_path,
                "// bbtool:init tier=regular fn=bb_wifi_prov_autoinit "
                "component=bb_target args=NULL\n",
            )
            via_manifest_out = str(root / "via_manifest" / "c.cmake")
            manifest_args = argparse.Namespace(
                root=str(root), components="bb_log,bb_meminfo", platform="espidf",
                components_out=via_manifest_out,
                wire_out=str(root / "via_manifest" / "bb_app_init.c"),
                consumer_manifest=str(manifest_path),
            )
            buf2 = io.StringIO()
            with contextlib.redirect_stdout(buf2):
                rc2 = run(manifest_args)
            self.assertEqual(rc2, 0)

            self.assertEqual(
                self._requires_set(direct_out), self._requires_set(via_manifest_out)
            )

    def test_manifest_component_field_unknown_component_errors_loudly(self):
        """An unknown/misspelled 'component=' name is a hard, clean error
        naming both the component and the manifest file -- never a silent
        skip, never a raw traceback."""
        with tempfile.TemporaryDirectory() as tmp:
            root = _fixture_root(tmp)
            manifest_path = root / "examples" / "smoke" / "main" / "smoke_app.c"
            manifest_path.parent.mkdir(parents=True)
            manifest_path.write_text(
                "// bbtool:init tier=regular fn=bb_ghost_autoinit "
                "component=bb_ghost args=NULL\n",
                encoding="utf-8",
            )
            args = argparse.Namespace(
                root=str(root), components="bb_log,bb_meminfo", platform="espidf",
                components_out=None, wire_out=None,
                consumer_manifest=str(manifest_path),
            )
            err_buf = io.StringIO()
            with contextlib.redirect_stderr(err_buf):
                rc = run(args)
            self.assertEqual(rc, 1)
            err = err_buf.getvalue()
            self.assertIn("bbtool codegen: error:", err)
            self.assertIn("bb_ghost", err)
            self.assertIn("smoke_app.c", err)

    def test_manifest_component_field_conflicts_with_board_remove_components_hard_errors(self):
        """Review finding 1: a manifest 'component=' must never silently
        override a board's own remove_components -- board 'native' removes
        bb_target, so a manifest naming component=bb_target on that same
        board is a hard, clean error naming the board, the component, and
        the manifest path."""
        with tempfile.TemporaryDirectory() as tmp:
            root = self._fixture_with_target(tmp)
            _write_toml(root, (
                '[capability.demo]\n'
                'components = ["bb_log", "bb_meminfo"]\n\n'
                '[board.native]\n'
                'capabilities = ["demo"]\n'
                'remove_components = ["bb_target"]\n'
            ))
            manifest_path = root / "main" / "smoke_app.c"
            _write(
                manifest_path,
                "// bbtool:init tier=regular fn=bb_wifi_prov_autoinit "
                "component=bb_target args=NULL\n",
            )
            args = argparse.Namespace(
                root=str(root), components=None, board="native", wire_board=None,
                platform="espidf", components_out=None, wire_out=None,
                consumer_manifest=str(manifest_path),
            )
            err_buf = io.StringIO()
            with contextlib.redirect_stderr(err_buf):
                rc = run(args)
            self.assertEqual(rc, 1)
            err = err_buf.getvalue()
            self.assertIn("bbtool codegen: error:", err)
            self.assertIn("bb_target", err)
            self.assertIn("native", err)
            self.assertIn("remove_components", err)
            self.assertIn("smoke_app.c", err)

    def test_manifest_component_field_removed_by_different_board_still_succeeds(self):
        """Negative case for finding 1: bb_target is removed by board
        'native' but board 'other' doesn't remove it -- building 'other'
        with the same manifest must still succeed and fold bb_target in."""
        with tempfile.TemporaryDirectory() as tmp:
            root = self._fixture_with_target(tmp)
            _write_toml(root, (
                '[capability.demo]\n'
                'components = ["bb_log", "bb_meminfo"]\n\n'
                '[board.native]\n'
                'capabilities = ["demo"]\n'
                'remove_components = ["bb_target"]\n\n'
                '[board.other]\n'
                'capabilities = ["demo"]\n'
            ))
            manifest_path = root / "main" / "smoke_app.c"
            _write(
                manifest_path,
                "// bbtool:init tier=regular fn=bb_wifi_prov_autoinit "
                "component=bb_target args=NULL\n",
            )
            components_out = str(root / "out" / "c.cmake")
            wire_out = str(root / "out" / "bb_app_init.c")
            args = argparse.Namespace(
                root=str(root), components=None, board="other", wire_board=None,
                platform="espidf", components_out=components_out, wire_out=wire_out,
                consumer_manifest=str(manifest_path),
            )
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                rc = run(args)
            self.assertEqual(rc, 0)
            self.assertIn("bb_target", self._requires_set(components_out))

    def test_manifest_component_field_transitive_removed_dependency_hard_errors(self):
        """Transitive follow-up to finding 1: board 'native' does NOT list
        bb_target itself in remove_components -- it removes bb_target_dep,
        a REQUIRES-only dependency the manifest-named bb_target pulls in
        transitively. component=bb_target must still hard-error, and the
        message must name the requires-chain from bb_target to
        bb_target_dep."""
        with tempfile.TemporaryDirectory() as tmp:
            root = self._fixture_with_target(tmp)
            _write_toml(root, (
                '[capability.demo]\n'
                'components = ["bb_log", "bb_meminfo"]\n\n'
                '[board.native]\n'
                'capabilities = ["demo"]\n'
                'remove_components = ["bb_target_dep"]\n'
            ))
            manifest_path = root / "main" / "smoke_app.c"
            _write(
                manifest_path,
                "// bbtool:init tier=regular fn=bb_wifi_prov_autoinit "
                "component=bb_target args=NULL\n",
            )
            args = argparse.Namespace(
                root=str(root), components=None, board="native", wire_board=None,
                platform="espidf", components_out=None, wire_out=None,
                consumer_manifest=str(manifest_path),
            )
            err_buf = io.StringIO()
            with contextlib.redirect_stderr(err_buf):
                rc = run(args)
            self.assertEqual(rc, 1)
            err = err_buf.getvalue()
            self.assertIn("bbtool codegen: error:", err)
            self.assertIn("bb_target_dep", err)
            self.assertIn("native", err)
            self.assertIn("remove_components", err)
            self.assertIn("bb_target -> bb_target_dep", err)

    def test_manifest_component_field_multiple_names_chain_traces_to_the_culprit(self):
        """Multi-named manifest: one entry declares component=bb_extra (an
        innocent, dependency-free component), a second declares
        component=bb_target on the SAME manifest file -- only bb_target
        pulls in the removed bb_target_dep. The error must fire and the
        requires-chain must name bb_target (the actual culprit), never the
        innocent bb_extra."""
        with tempfile.TemporaryDirectory() as tmp:
            root = self._fixture_with_target(tmp)
            _make_component(root, "bb_extra", "#pragma once\nbb_err_t bb_extra_noop(void);\n")
            _write_toml(root, (
                '[capability.demo]\n'
                'components = ["bb_log", "bb_meminfo"]\n\n'
                '[board.native]\n'
                'capabilities = ["demo"]\n'
                'remove_components = ["bb_target_dep"]\n'
            ))
            manifest_path = root / "main" / "smoke_app.c"
            _write(
                manifest_path,
                "// bbtool:init tier=regular fn=bb_extra_noop component=bb_extra args=NULL\n"
                "// bbtool:init tier=regular fn=bb_wifi_prov_autoinit "
                "component=bb_target args=NULL\n",
            )
            args = argparse.Namespace(
                root=str(root), components=None, board="native", wire_board=None,
                platform="espidf", components_out=None, wire_out=None,
                consumer_manifest=str(manifest_path),
            )
            err_buf = io.StringIO()
            with contextlib.redirect_stderr(err_buf):
                rc = run(args)
            self.assertEqual(rc, 1)
            err = err_buf.getvalue()
            self.assertIn("bbtool codegen: error:", err)
            self.assertIn("bb_target -> bb_target_dep", err)
            self.assertNotIn("bb_extra ->", err)

    def test_manifest_component_field_removed_component_already_in_base_closure_does_not_error(self):
        """Negative case guarding against the naive fix (a blanket
        intersection over the FULL resolved closure): board 'native's own
        capability set already transitively pulls bb_target_dep in via
        bb_target (remove_components nets out the flat REQUESTED set, not
        the transitive closure, so listing bb_target_dep there is a no-op
        against a dependency only ever reached transitively) -- that's
        pre-existing, valid behavior. A manifest naming a completely
        unrelated, dependency-free component must NOT newly error just
        because bb_target_dep also happens to be in remove_components --
        it was never introduced by the manifest fold (not in the delta)."""
        with tempfile.TemporaryDirectory() as tmp:
            root = self._fixture_with_target(tmp)
            _make_component(root, "bb_extra", "#pragma once\nbb_err_t bb_extra_noop(void);\n")
            _write_toml(root, (
                '[capability.demo]\n'
                'components = ["bb_log", "bb_meminfo", "bb_target"]\n\n'
                '[board.native]\n'
                'capabilities = ["demo"]\n'
                'remove_components = ["bb_target_dep"]\n'
            ))
            manifest_path = root / "main" / "smoke_app.c"
            _write(
                manifest_path,
                "// bbtool:init tier=regular fn=bb_extra_noop "
                "component=bb_extra args=NULL\n",
            )
            components_out = str(root / "out" / "c.cmake")
            wire_out = str(root / "out" / "bb_app_init.c")
            args = argparse.Namespace(
                root=str(root), components=None, board="native", wire_board=None,
                platform="espidf", components_out=components_out, wire_out=wire_out,
                consumer_manifest=str(manifest_path),
            )
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                rc = run(args)
            self.assertEqual(rc, 0)
            resolved = self._requires_set(components_out)
            self.assertIn("bb_target_dep", resolved)
            self.assertIn("bb_extra", resolved)

    def test_manifest_component_field_does_not_scan_named_components_own_markers(self):
        """Review finding 2 regression: the manifest-named component may
        carry its OWN real '// bbtool:init' marker -- codegen must still
        fold it into REQUIRES without scanning that marker (only the
        manifest entry's own marker gets wired). Every other fixture in
        this class uses a marker-free bb_target, which can't catch a
        regression of the wire_names/names aliasing documented at its
        capture site in codegen.run()."""
        with tempfile.TemporaryDirectory() as tmp:
            root = _fixture_root(tmp)
            _make_component(
                root, "bb_target_dep", "#pragma once\nbb_err_t bb_target_dep_init(void);\n",
            )
            _make_component(
                root, "bb_target",
                "#pragma once\n"
                "// bbtool:init tier=early fn=bb_target_own_init\n"
                "bb_err_t bb_target_own_init(void);\n"
                "bb_err_t bb_wifi_prov_autoinit(void);\n",
                requires=["bb_target_dep"],
            )
            manifest_path = root / "main" / "smoke_app.c"
            _write(
                manifest_path,
                "// bbtool:init tier=regular fn=bb_wifi_prov_autoinit "
                "component=bb_target args=NULL\n",
            )
            components_out = str(root / "out" / "c.cmake")
            wire_out = str(root / "out" / "bb_app_init.c")
            args = argparse.Namespace(
                root=str(root), components="bb_log,bb_meminfo", platform="espidf",
                components_out=components_out, wire_out=wire_out,
                consumer_manifest=str(manifest_path),
            )
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                rc = run(args)
            self.assertEqual(rc, 0)
            self.assertIn("bb_target", self._requires_set(components_out))
            source = Path(wire_out).read_text(encoding="utf-8")
            self.assertIn("bb_wifi_prov_autoinit(NULL)", source)
            self.assertNotIn("bb_target_own_init", source)

    def test_manifest_without_component_field_byte_identical(self):
        """A manifest entry with NO 'component=' field must not perturb the
        REQUIRES closure at all -- byte-identical to --components alone."""
        with tempfile.TemporaryDirectory() as tmp:
            root = _fixture_root(tmp)
            no_manifest_out = str(root / "no_manifest" / "c.cmake")
            args1 = argparse.Namespace(
                root=str(root), components="bb_log,bb_meminfo", platform="espidf",
                components_out=no_manifest_out,
                wire_out=str(root / "no_manifest" / "bb_app_init.c"),
            )
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                rc = run(args1)
            self.assertEqual(rc, 0)

            manifest_path = root / "main" / "smoke_app.c"
            _write(
                manifest_path,
                "// bbtool:init tier=early fn=smoke_app_start requires=log_stream\n",
            )
            with_manifest_out = str(root / "with_manifest" / "c.cmake")
            args2 = argparse.Namespace(
                root=str(root), components="bb_log,bb_meminfo", platform="espidf",
                components_out=with_manifest_out,
                wire_out=str(root / "with_manifest" / "bb_app_init.c"),
                consumer_manifest=str(manifest_path),
            )
            buf2 = io.StringIO()
            with contextlib.redirect_stdout(buf2):
                rc2 = run(args2)
            self.assertEqual(rc2, 0)

            self.assertEqual(
                Path(no_manifest_out).read_text(encoding="utf-8"),
                Path(with_manifest_out).read_text(encoding="utf-8"),
            )


class _FakeEnv:
    """Minimal stand-in for SCons' env object (mirrors test_scaffold.py's
    `_FakeEnv`): `pio_main` only ever calls `Exit()` on error (never
    Append()/get() -- unlike scaffold.pio_main, this path doesn't mutate
    build flags), so `Exit()` recording is all this needs."""

    def __init__(self):
        self.exited_with = None

    def Exit(self, code):
        self.exited_with = code


class TestPioMain(unittest.TestCase):
    """FINDING 2: `codegen.pio_main` (B1-1084 Fork 3, not yet wired into
    `bbtool_pio.py` -- see the module docstring) had zero test coverage.
    Covers the success path (both artifacts written, `[discovery]`
    `extra_roots` resolved relative to the CONSUMER root -- `root`, the arg
    `pio_main` is called with -- exactly as its own docstring claims) plus
    every `except (...)  -> env.Exit(1)` branch reachable from this
    function's own body."""

    def _consumer_and_extra(self, tmp: str):
        root = Path(tmp) / "consumer"
        extra = Path(tmp) / "extra"
        _write(root / ".keep", "")
        return root, extra

    def test_pio_main_writes_both_artifacts_and_resolves_extra_root_against_consumer_dir(self):
        with tempfile.TemporaryDirectory() as tmp:
            root, extra = self._consumer_and_extra(tmp)
            _make_component(
                extra, "bb_pio_fixture",
                "#pragma once\n"
                "// bbtool:init tier=early fn=bb_pio_fixture_init\n"
                "bb_err_t bb_pio_fixture_init(void);\n",
            )
            # extra_roots resolved relative to `root` (pio_main's own
            # docstring convention) -- NOT cwd, NOT bb_root.
            rel = os.path.relpath(str(extra), str(root))
            config = {
                "discovery": {"extra_roots": [rel]},
                "capability": {},
                "board": {"native": {"platform": "host", "add_components": ["bb_pio_fixture"]}},
            }
            env = _FakeEnv()
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                pio_main(env, str(root), "native", config)
            self.assertIsNone(env.exited_with)

            components_out = root / "examples" / "smoke" / "main" / "generated" / "bb_autowire_components.cmake"
            wire_out = root / "main" / "generated" / "bb_app_init.c"
            wire_cmake_out = root / "main" / "generated" / "bb_app_init.cmake"
            self.assertTrue(components_out.is_file())
            self.assertTrue(wire_out.is_file())
            self.assertTrue(wire_cmake_out.is_file())
            self.assertIn("bb_pio_fixture", components_out.read_text(encoding="utf-8"))
            self.assertIn("bb_pio_fixture_init()", wire_out.read_text(encoding="utf-8"))
            self.assertIn("bb_codegen: wrote", buf.getvalue())

    def test_pio_main_exits_on_unknown_board(self):
        """ManifestError branch #1: `load_manifest`/`resolve_component_names`
        raises for a board absent from the (empty) manifest."""
        with tempfile.TemporaryDirectory() as tmp:
            root, _extra = self._consumer_and_extra(tmp)
            env = _FakeEnv()
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                pio_main(env, str(root), "ghost", {"capability": {}, "board": {}})
            self.assertEqual(env.exited_with, 1)

    def test_pio_main_exits_when_board_resolves_no_components(self):
        """ManifestError branch #2: pio_main's own explicit `if not names:
        raise ManifestError(...)` -- a real board with zero active
        capabilities/add_components."""
        with tempfile.TemporaryDirectory() as tmp:
            root, _extra = self._consumer_and_extra(tmp)
            config = {"capability": {}, "board": {"empty": {"platform": "host"}}}
            env = _FakeEnv()
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                pio_main(env, str(root), "empty", config)
            self.assertEqual(env.exited_with, 1)

    def test_pio_main_exits_on_missing_provider(self):
        """A DIFFERENT exception type in the same except tuple
        (`wire_graph.MissingProviderError`, via a `requires=` marker with no
        matching provider in the resolved set) -- exercises the tail of the
        try block (collect_entries/collect_provides_entries/topo_sort), not
        just the earlier manifest-resolution lines."""
        with tempfile.TemporaryDirectory() as tmp:
            root, extra = self._consumer_and_extra(tmp)
            _make_component(
                extra, "bb_pio_bad",
                "#pragma once\n"
                "// bbtool:init tier=early fn=bb_pio_bad_init requires=ghost_token\n"
                "bb_err_t bb_pio_bad_init(void);\n",
            )
            rel = os.path.relpath(str(extra), str(root))
            config = {
                "discovery": {"extra_roots": [rel]},
                "capability": {},
                "board": {"native": {"platform": "host", "add_components": ["bb_pio_bad"]}},
            }
            env = _FakeEnv()
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                pio_main(env, str(root), "native", config)
            self.assertEqual(env.exited_with, 1)


def _real_bb_data_max_bindings() -> int:
    """Reads the REAL, committed `BB_DATA_MAX_BINDINGS` value from this
    checkout's own `components/bb_data/include/bb_data.h` -- the same file
    `pio_main`'s own `bb_root` derivation (`_THIS_DIR/../../../`) resolves
    to, since this test file and `commands/codegen.py` are siblings under
    `scripts/bbtool/`. Read dynamically rather than hardcoded so this test
    never silently stops testing the over-cap path if the real constant is
    ever raised/lowered."""
    bb_root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
    header = os.path.join(bb_root, "components", "bb_data", "include", "bb_data.h")
    with open(header, "r", encoding="utf-8") as f:
        for line in f:
            stripped = line.strip()
            if stripped.startswith("#define"):
                parts = stripped.split()
                if len(parts) >= 3 and parts[1] == "BB_DATA_MAX_BINDINGS":
                    return int(parts[2])
    raise AssertionError(f"{header}: no BB_DATA_MAX_BINDINGS #define found")


class TestBindsDataCapIntegration(unittest.TestCase):
    """MEDIUM 2: proves `check_binds_data_cap` is actually WIRED into both
    CLI entry points (`run()`/`pio_main()`), not merely unit-testable in
    isolation against a hand-built `ordered`/`roots` -- the exact defect
    class this whole epic exists to fix (an unwired guard that every
    direct-call test still passes). Each test below was manually verified
    to FAIL when its corresponding call-site line is removed from
    `commands/codegen.py` (see the session report for the verbatim
    failures); this class stays as the regression pin for that wiring."""

    def _binders_header(self, count: int) -> str:
        """One `binds_data=` key per entry -- `count` entries == `count`
        keys, so this fixture's cap math is unchanged post-B1-1355 (sum of
        1-key entries equals the old entry count)."""
        body = "#pragma once\n"
        for i in range(count):
            body += (
                f"// bbtool:init tier=regular fn=bb_binder_{i}_init "
                f"binds_data=key_{i}\n"
                f"bb_err_t bb_binder_{i}_init(void);\n"
            )
        return body

    def test_run_propagates_over_cap_wire_error_as_exit_1(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            cap = 2
            _make_component(root, "bb_binders", self._binders_header(cap + 1))
            _write(
                root / "components" / "bb_data" / "include" / "bb_data.h",
                f"#pragma once\n#define BB_DATA_MAX_BINDINGS {cap}\n",
            )
            components_out = str(root / "out" / "bb_autowire_components.cmake")
            wire_out = str(root / "out" / "bb_app_init.c")
            args = argparse.Namespace(
                root=str(root), components="bb_binders",
                platform="espidf", components_out=components_out, wire_out=wire_out,
            )
            out_buf, err_buf = io.StringIO(), io.StringIO()
            with contextlib.redirect_stdout(out_buf), contextlib.redirect_stderr(err_buf):
                rc = run(args)
            self.assertEqual(rc, 1)
            self.assertIn("bb_data key(s) are bound across", err_buf.getvalue())
            self.assertIn("BB_DATA_MAX_BINDINGS", err_buf.getvalue())

    def test_pio_main_propagates_over_cap_wire_error_as_exit_1(self):
        """Relies on THIS checkout's real `components/bb_data/include/
        bb_data.h` (`pio_main`'s own `bb_root` always resolves there --
        see `_real_bb_data_max_bindings`'s docstring), so no fixture
        bb_data.h is written here -- proves the wiring against the real
        header path, not a synthetic stand-in."""
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "consumer"
            extra = Path(tmp) / "extra"
            _write(root / ".keep", "")
            cap = _real_bb_data_max_bindings()
            _make_component(extra, "bb_pio_binders", self._binders_header(cap + 1))
            rel = os.path.relpath(str(extra), str(root))
            config = {
                "discovery": {"extra_roots": [rel]},
                "capability": {},
                "board": {"native": {"platform": "host", "add_components": ["bb_pio_binders"]}},
            }
            env = _FakeEnv()
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                pio_main(env, str(root), "native", config)
            self.assertEqual(env.exited_with, 1)


if __name__ == "__main__":
    unittest.main()
