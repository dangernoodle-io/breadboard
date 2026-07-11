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

from commands.codegen import run


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


if __name__ == "__main__":
    unittest.main()
