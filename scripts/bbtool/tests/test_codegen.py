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


if __name__ == "__main__":
    unittest.main()
