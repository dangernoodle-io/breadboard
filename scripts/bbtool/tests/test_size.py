"""size command tests: pure `size`-output + .map-file parsing logic (no
toolchain/hardware required), plus the `run()` CLI entry point exercised via
mocked subprocess/toolchain-discovery calls."""
import argparse
import contextlib
import hashlib
import io
import json
import os
import subprocess
import sys
import tempfile
import unittest
import urllib.error
from pathlib import Path
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "commands"))

from commands import size as size_mod
from commands.size import (
    baseline_path,
    parse_map_component_sizes,
    parse_size_output,
    run,
)


class TestParseSizeOutput(unittest.TestCase):
    def test_parses_berkeley_format(self):
        out = (
            "   text\t   data\t    bss\t    dec\t    hex\tfilename\n"
            "  12345\t    678\t   9012\t  21995\t   55eb\tfirmware.elf\n"
        )
        result = parse_size_output(out)
        self.assertEqual(result, {"text": 12345, "data": 678, "bss": 9012, "dec": 21995})

    def test_raises_on_missing_data_line(self):
        with self.assertRaises(ValueError):
            parse_size_output("   text\t   data\t    bss\n")

    def test_raises_on_short_data_line(self):
        with self.assertRaises(ValueError):
            parse_size_output("header\n1 2\n")


class TestParseMapComponentSizes(unittest.TestCase):
    def test_sums_bytes_per_archive_member(self):
        map_text = (
            " .text.bb_wifi_init\n"
            "                0x0000000040123456      0x120 .pio/build/esp32/esp-idf/bb_wifi/libbb_wifi.a(bb_wifi.c.o)\n"
            " .rodata.bb_wifi_tag\n"
            "                0x0000000040123576       0x10 .pio/build/esp32/esp-idf/bb_wifi/libbb_wifi.a(bb_wifi.c.o)\n"
            " .text.bb_log_i\n"
            "                0x0000000040123586       0x40 .pio/build/esp32/esp-idf/bb_log/libbb_log.a(bb_log.c.o)\n"
        )
        sizes = parse_map_component_sizes(map_text)
        self.assertEqual(sizes["bb_wifi"], 0x120 + 0x10)
        self.assertEqual(sizes["bb_log"], 0x40)

    def test_filters_to_requested_components_only(self):
        map_text = (
            " .text.bb_wifi_init\n"
            "                0x0000000040123456      0x120 lib/libbb_wifi.a(a.c.o)\n"
            " .text.bb_log_i\n"
            "                0x0000000040123586       0x40 lib/libbb_log.a(b.c.o)\n"
        )
        sizes = parse_map_component_sizes(map_text, components=["bb_wifi"])
        self.assertEqual(sizes, {"bb_wifi": 0x120})

    def test_absent_component_has_no_entry(self):
        map_text = " .text.x\n                0x1 0x10 lib/libbb_log.a(a.c.o)\n"
        sizes = parse_map_component_sizes(map_text, components=["bb_wifi"])
        self.assertNotIn("bb_wifi", sizes)
        self.assertEqual(sizes, {})

    def test_empty_map_text_returns_empty_dict(self):
        self.assertEqual(parse_map_component_sizes(""), {})


_BERKELEY_OUT = (
    "   text\t   data\t    bss\t    dec\t    hex\tfilename\n"
    "  12345\t    678\t   9012\t  21995\t   55eb\tfirmware.elf\n"
)


def _args(build_dir, **overrides):
    base = dict(
        build_dir=build_dir, elf_path=None, map_path=None,
        components=None, arch="xtensa", size_tool_path=None,
        root=None, target=None, update_baseline=False, check=False,
        flash_threshold_pct=2.0, heap_from_http=None,
    )
    base.update(overrides)
    return argparse.Namespace(**base)


class TestRunCli(unittest.TestCase):
    def test_run_missing_elf_returns_error(self):
        with tempfile.TemporaryDirectory() as tmp:
            buf = io.StringIO()
            with contextlib.redirect_stderr(buf):
                rc = run(_args(tmp))
            self.assertEqual(rc, 1)
            self.assertIn("ELF not found", buf.getvalue())

    def test_run_no_size_tool_found_returns_error(self):
        with tempfile.TemporaryDirectory() as tmp:
            Path(tmp, "firmware.elf").write_bytes(b"\x7fELF")
            with mock.patch("commands.size.find_size_tool", return_value=None):
                buf = io.StringIO()
                with contextlib.redirect_stderr(buf):
                    rc = run(_args(tmp))
            self.assertEqual(rc, 1)
            self.assertIn("no size tool found", buf.getvalue())

    def test_run_size_tool_called_process_error_returns_error(self):
        with tempfile.TemporaryDirectory() as tmp:
            Path(tmp, "firmware.elf").write_bytes(b"\x7fELF")
            with mock.patch("commands.size.find_size_tool", return_value="/fake/size"), \
                 mock.patch(
                     "commands.size.subprocess.run",
                     side_effect=subprocess.CalledProcessError(1, "/fake/size", stderr="boom"),
                 ):
                buf = io.StringIO()
                with contextlib.redirect_stderr(buf):
                    rc = run(_args(tmp))
            self.assertEqual(rc, 1)
            self.assertIn("failed", buf.getvalue())

    def test_run_size_tool_timeout_returns_error(self):
        with tempfile.TemporaryDirectory() as tmp:
            Path(tmp, "firmware.elf").write_bytes(b"\x7fELF")
            with mock.patch("commands.size.find_size_tool", return_value="/fake/size"), \
                 mock.patch(
                     "commands.size.subprocess.run",
                     side_effect=subprocess.TimeoutExpired("/fake/size", 30),
                 ):
                buf = io.StringIO()
                with contextlib.redirect_stderr(buf):
                    rc = run(_args(tmp))
            self.assertEqual(rc, 1)
            self.assertIn("timed out", buf.getvalue())

    def test_run_unparseable_size_output_returns_error(self):
        with tempfile.TemporaryDirectory() as tmp:
            Path(tmp, "firmware.elf").write_bytes(b"\x7fELF")
            fake_proc = subprocess.CompletedProcess(args=["/fake/size"], returncode=0, stdout="garbage\n")
            with mock.patch("commands.size.find_size_tool", return_value="/fake/size"), \
                 mock.patch("commands.size.subprocess.run", return_value=fake_proc):
                buf = io.StringIO()
                with contextlib.redirect_stderr(buf):
                    rc = run(_args(tmp))
            self.assertEqual(rc, 1)

    def test_run_warns_on_missing_map_even_without_component_filter(self):
        with tempfile.TemporaryDirectory() as tmp:
            Path(tmp, "firmware.elf").write_bytes(b"\x7fELF")
            fake_proc = subprocess.CompletedProcess(args=["/fake/size"], returncode=0, stdout=_BERKELEY_OUT)
            with mock.patch("commands.size.find_size_tool", return_value="/fake/size"), \
                 mock.patch("commands.size.subprocess.run", return_value=fake_proc):
                out_buf, err_buf = io.StringIO(), io.StringIO()
                with contextlib.redirect_stdout(out_buf), contextlib.redirect_stderr(err_buf):
                    rc = run(_args(tmp, components=None))
            self.assertEqual(rc, 0)
            self.assertIn(".map not found", err_buf.getvalue())

    def test_run_success_emits_json_with_flash_total(self):
        with tempfile.TemporaryDirectory() as tmp:
            Path(tmp, "firmware.elf").write_bytes(b"\x7fELF")
            Path(tmp, "firmware.map").write_text(
                " .text.bb_wifi_init\n"
                "                0x0000000040123456      0x120 lib/libbb_wifi.a(a.c.o)\n",
                encoding="utf-8",
            )
            fake_proc = subprocess.CompletedProcess(args=["/fake/size"], returncode=0, stdout=_BERKELEY_OUT)
            with mock.patch("commands.size.find_size_tool", return_value="/fake/size"), \
                 mock.patch("commands.size.subprocess.run", return_value=fake_proc) as run_mock:
                out_buf = io.StringIO()
                with contextlib.redirect_stdout(out_buf):
                    rc = run(_args(tmp, components=["bb_wifi"]))
            self.assertEqual(rc, 0)
            # --format=berkeley must be explicit -- don't rely on the resolved
            # tool's compiled-in default (e.g. llvm-size defaults to SysV).
            called_argv = run_mock.call_args[0][0]
            self.assertIn("--format=berkeley", called_argv)
            payload = out_buf.getvalue()
            self.assertIn('"flash_total": 13023', payload)
            self.assertIn('"bb_wifi": 288', payload)

    def test_single_shot_heap_from_http_warns_but_still_emits_json(self):
        # --heap-from-http is a no-op without --update-baseline/--check;
        # single-shot mode must warn (not silently ignore) but still emit
        # the normal JSON result at rc=0.
        with tempfile.TemporaryDirectory() as tmp:
            Path(tmp, "firmware.elf").write_bytes(b"\x7fELF")
            fake_proc = subprocess.CompletedProcess(args=["/fake/size"], returncode=0, stdout=_BERKELEY_OUT)
            with mock.patch("commands.size.find_size_tool", return_value="/fake/size"), \
                 mock.patch("commands.size.subprocess.run", return_value=fake_proc):
                out_buf, err_buf = io.StringIO(), io.StringIO()
                with contextlib.redirect_stdout(out_buf), contextlib.redirect_stderr(err_buf):
                    rc = run(_args(tmp, heap_from_http="http://example-device.test"))
            self.assertEqual(rc, 0)
            self.assertIn("--heap-from-http", err_buf.getvalue())
            self.assertIn("ignored", err_buf.getvalue())
            self.assertIn('"flash_total": 13023', out_buf.getvalue())

    def test_single_shot_without_heap_from_http_no_warning(self):
        with tempfile.TemporaryDirectory() as tmp:
            Path(tmp, "firmware.elf").write_bytes(b"\x7fELF")
            fake_proc = subprocess.CompletedProcess(args=["/fake/size"], returncode=0, stdout=_BERKELEY_OUT)
            with mock.patch("commands.size.find_size_tool", return_value="/fake/size"), \
                 mock.patch("commands.size.subprocess.run", return_value=fake_proc):
                err_buf = io.StringIO()
                with contextlib.redirect_stderr(err_buf):
                    rc = run(_args(tmp))
            self.assertEqual(rc, 0)
            self.assertNotIn("--heap-from-http", err_buf.getvalue())


class TestNormalizeSdkconfig(unittest.TestCase):
    def test_set_and_unset_lines_normalized(self):
        text = (
            "#\n"
            "# some header comment\n"
            "#\n"
            "CONFIG_LWIP_IPV6=y\n"
            "# CONFIG_BB_PSRAM_ENABLE is not set\n"
            "CONFIG_LOG_DEFAULT_LEVEL=3\n"
            "\n"
        )
        normalized = size_mod._normalize_sdkconfig(text)
        self.assertIn("CONFIG_LWIP_IPV6=y", normalized.splitlines())
        self.assertIn("CONFIG_BB_PSRAM_ENABLE=n", normalized.splitlines())
        self.assertIn("CONFIG_LOG_DEFAULT_LEVEL=3", normalized.splitlines())
        # header comments dropped
        self.assertNotIn("some header comment", normalized)

    def test_sorted_and_deterministic(self):
        text = "CONFIG_Z=1\nCONFIG_A=1\nCONFIG_M=1\n"
        normalized = size_mod._normalize_sdkconfig(text)
        lines = normalized.splitlines()
        self.assertEqual(lines, sorted(lines))

    def test_stable_sha_for_equivalent_input(self):
        text_a = "CONFIG_A=y\n# CONFIG_B is not set\n"
        text_b = "# CONFIG_B is not set\nCONFIG_A=y\n"
        norm_a = size_mod._normalize_sdkconfig(text_a)
        norm_b = size_mod._normalize_sdkconfig(text_b)
        self.assertEqual(norm_a, norm_b)

    def test_empty_input_yields_empty_normalized_text(self):
        self.assertEqual(size_mod._normalize_sdkconfig(""), "\n")


class TestFindSdkconfig(unittest.TestCase):
    def test_finds_build_dir_copy(self):
        with tempfile.TemporaryDirectory() as tmp:
            build_dir = Path(tmp, "examples", "smoke", ".pio", "build", "esp32")
            build_dir.mkdir(parents=True)
            (build_dir / "sdkconfig").write_text("CONFIG_A=y\n", encoding="utf-8")
            found = size_mod._find_sdkconfig(build_dir)
            self.assertEqual(found, build_dir / "sdkconfig")

    def test_finds_project_root_env_specific_copy(self):
        with tempfile.TemporaryDirectory() as tmp:
            project_dir = Path(tmp, "examples", "smoke")
            build_dir = project_dir / ".pio" / "build" / "esp32"
            build_dir.mkdir(parents=True)
            (project_dir / "sdkconfig.esp32").write_text("CONFIG_A=y\n", encoding="utf-8")
            found = size_mod._find_sdkconfig(build_dir)
            self.assertEqual(found, project_dir / "sdkconfig.esp32")

    def test_returns_none_when_absent(self):
        with tempfile.TemporaryDirectory() as tmp:
            build_dir = Path(tmp, "examples", "smoke", ".pio", "build", "esp32")
            build_dir.mkdir(parents=True)
            self.assertIsNone(size_mod._find_sdkconfig(build_dir))


class TestSnapshotConfig(unittest.TestCase):
    def test_writes_snapshot_and_returns_sha_and_relpath(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = tmp
            build_dir = Path(tmp, "examples", "smoke", ".pio", "build", "esp32")
            build_dir.mkdir(parents=True)
            (build_dir / "sdkconfig").write_text(
                "CONFIG_A=y\n# CONFIG_B is not set\n", encoding="utf-8",
            )
            sha, rel = size_mod._snapshot_config(str(build_dir), "esp32", root)
            self.assertEqual(rel, ".baseline/bbtool/metrics/esp32.sdkconfig")
            snapshot_path = Path(root, rel)
            self.assertTrue(snapshot_path.is_file())
            content = snapshot_path.read_text(encoding="utf-8")
            self.assertIn("CONFIG_A=y", content)
            self.assertIn("CONFIG_B=n", content)
            expected_sha = hashlib.sha256(content.encode("utf-8")).hexdigest()
            self.assertEqual(sha, expected_sha)


class TestUpdateBaseline(unittest.TestCase):
    def _fake_measure(self, result):
        return mock.patch(
            "commands.size._measure",
            return_value=(result, None, None),
        )

    def test_writes_baseline_json(self):
        with tempfile.TemporaryDirectory() as tmp:
            build_dir = Path(tmp, "examples", "smoke", ".pio", "build", "esp32")
            build_dir.mkdir(parents=True)
            Path(build_dir, "firmware.elf").write_bytes(b"\x7fELF")
            result = {
                "elf": str(build_dir / "firmware.elf"), "build_dir": str(build_dir),
                "text": 100, "data": 20, "bss": 30, "flash_total": 120,
                "components": {"bb_core": 50},
            }
            with self._fake_measure(result):
                rc = run(_args(str(build_dir), root=tmp, target="esp32", update_baseline=True))
            self.assertEqual(rc, 0)
            baseline = json.loads(baseline_path(tmp, "esp32").read_text(encoding="utf-8"))
            self.assertEqual(baseline["target"], "esp32")
            self.assertEqual(baseline["flash"]["flash_total"], 120)
            self.assertEqual(baseline["flash"]["bss"], 30)
            self.assertEqual(baseline["flash"]["components"], {"bb_core": 50})
            self.assertEqual(baseline["heap"], size_mod._HEAP_NULL)
            self.assertIn("sdkconfig_sha", baseline["config"])

    def test_preserves_existing_heap_block_on_rewrite(self):
        with tempfile.TemporaryDirectory() as tmp:
            build_dir = Path(tmp, "examples", "smoke", ".pio", "build", "esp32")
            build_dir.mkdir(parents=True)
            Path(build_dir, "firmware.elf").write_bytes(b"\x7fELF")
            existing_heap = {"min_free": 12345, "high_water": 20000, "regions": ["dram"], "source": "device"}
            existing_baseline = {
                "target": "esp32", "arch": "xtensa",
                "config": {"label": "default", "toolchain": "esp-idf", "sdkconfig_sha": "old", "snapshot": "x"},
                "flash": {"text": 1, "data": 1, "bss": 1, "flash_total": 2, "components": {}},
                "heap": existing_heap,
            }
            path = baseline_path(tmp, "esp32")
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(json.dumps(existing_baseline), encoding="utf-8")

            result = {
                "elf": str(build_dir / "firmware.elf"), "build_dir": str(build_dir),
                "text": 100, "data": 20, "bss": 30, "flash_total": 120,
                "components": {"bb_core": 50},
            }
            with self._fake_measure(result):
                rc = run(_args(str(build_dir), root=tmp, target="esp32", update_baseline=True))
            self.assertEqual(rc, 0)
            baseline = json.loads(path.read_text(encoding="utf-8"))
            self.assertEqual(baseline["heap"], existing_heap)
            self.assertEqual(baseline["flash"]["flash_total"], 120)

    def test_roundtrip_via_load_baseline(self):
        with tempfile.TemporaryDirectory() as tmp:
            build_dir = Path(tmp, "examples", "smoke", ".pio", "build", "esp32")
            build_dir.mkdir(parents=True)
            Path(build_dir, "firmware.elf").write_bytes(b"\x7fELF")
            result = {
                "elf": str(build_dir / "firmware.elf"), "build_dir": str(build_dir),
                "text": 100, "data": 20, "bss": 30, "flash_total": 120,
                "components": {},
            }
            with self._fake_measure(result):
                run(_args(str(build_dir), root=tmp, target="esp32", update_baseline=True))
            loaded = size_mod._load_baseline(tmp, "esp32")
            self.assertEqual(loaded["flash"]["text"], 100)


class TestCheckFlashGate(unittest.TestCase):
    def _fake_measure(self, result):
        return mock.patch(
            "commands.size._measure",
            return_value=(result, None, None),
        )

    def _write_baseline(self, tmp, target, text, data, bss, flash_total, components=None):
        payload = {
            "target": target, "arch": "xtensa",
            "config": {"label": "default", "toolchain": "esp-idf", "sdkconfig_sha": "x", "snapshot": "x"},
            "flash": {
                "text": text, "data": data, "bss": bss,
                "flash_total": flash_total, "components": components or {},
            },
            "heap": size_mod._HEAP_NULL,
        }
        path = baseline_path(tmp, target)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(payload), encoding="utf-8")

    def _build_dir(self, tmp):
        build_dir = Path(tmp, "examples", "smoke", ".pio", "build", "esp32")
        build_dir.mkdir(parents=True)
        Path(build_dir, "firmware.elf").write_bytes(b"\x7fELF")
        return build_dir

    def test_check_missing_baseline_fails(self):
        with tempfile.TemporaryDirectory() as tmp:
            build_dir = self._build_dir(tmp)
            result = {
                "elf": str(build_dir / "firmware.elf"), "build_dir": str(build_dir),
                "text": 100, "data": 20, "bss": 30, "flash_total": 120, "components": {},
            }
            with self._fake_measure(result):
                buf = io.StringIO()
                with contextlib.redirect_stderr(buf):
                    rc = run(_args(str(build_dir), root=tmp, target="esp32", check=True))
            self.assertEqual(rc, 1)
            self.assertIn("no baseline", buf.getvalue())

    def test_check_bss_growth_fails(self):
        with tempfile.TemporaryDirectory() as tmp:
            build_dir = self._build_dir(tmp)
            self._write_baseline(tmp, "esp32", text=100, data=20, bss=30, flash_total=120)
            result = {
                "elf": str(build_dir / "firmware.elf"), "build_dir": str(build_dir),
                "text": 100, "data": 20, "bss": 31, "flash_total": 120, "components": {},
            }
            with self._fake_measure(result):
                buf = io.StringIO()
                with contextlib.redirect_stderr(buf):
                    rc = run(_args(str(build_dir), root=tmp, target="esp32", check=True))
            self.assertEqual(rc, 1)
            self.assertIn("bss grew", buf.getvalue())

    def test_check_flash_within_threshold_passes(self):
        with tempfile.TemporaryDirectory() as tmp:
            build_dir = self._build_dir(tmp)
            self._write_baseline(tmp, "esp32", text=1000, data=0, bss=30, flash_total=1000)
            # +1% growth, under the default 2% threshold
            result = {
                "elf": str(build_dir / "firmware.elf"), "build_dir": str(build_dir),
                "text": 1010, "data": 0, "bss": 30, "flash_total": 1010, "components": {},
            }
            with self._fake_measure(result):
                out_buf = io.StringIO()
                with contextlib.redirect_stdout(out_buf):
                    rc = run(_args(str(build_dir), root=tmp, target="esp32", check=True))
            self.assertEqual(rc, 0)
            self.assertIn("PASS", out_buf.getvalue())

    def test_check_flash_over_threshold_fails(self):
        with tempfile.TemporaryDirectory() as tmp:
            build_dir = self._build_dir(tmp)
            self._write_baseline(tmp, "esp32", text=1000, data=0, bss=30, flash_total=1000)
            # +5% growth, over the default 2% threshold
            result = {
                "elf": str(build_dir / "firmware.elf"), "build_dir": str(build_dir),
                "text": 1050, "data": 0, "bss": 30, "flash_total": 1050, "components": {},
            }
            with self._fake_measure(result):
                buf = io.StringIO()
                with contextlib.redirect_stderr(buf):
                    rc = run(_args(str(build_dir), root=tmp, target="esp32", check=True))
            self.assertEqual(rc, 1)
            self.assertIn("FAIL flash_total grew", buf.getvalue())

    def test_check_flash_shrink_always_passes(self):
        with tempfile.TemporaryDirectory() as tmp:
            build_dir = self._build_dir(tmp)
            self._write_baseline(tmp, "esp32", text=1000, data=0, bss=30, flash_total=1000)
            result = {
                "elf": str(build_dir / "firmware.elf"), "build_dir": str(build_dir),
                "text": 900, "data": 0, "bss": 30, "flash_total": 900, "components": {},
            }
            with self._fake_measure(result):
                rc = run(_args(str(build_dir), root=tmp, target="esp32", check=True))
            self.assertEqual(rc, 0)

    def test_check_and_update_baseline_mutually_exclusive(self):
        with tempfile.TemporaryDirectory() as tmp:
            build_dir = self._build_dir(tmp)
            buf = io.StringIO()
            with contextlib.redirect_stderr(buf):
                rc = run(_args(str(build_dir), root=tmp, target="esp32", check=True, update_baseline=True))
            self.assertEqual(rc, 1)
            self.assertIn("mutually exclusive", buf.getvalue())

    def test_check_flash_exactly_at_threshold_passes(self):
        # delta/base * 100 == threshold_pct exactly -- _compare_flash uses
        # `pct > threshold_pct`, so an exact-equal pct must PASS.
        with tempfile.TemporaryDirectory() as tmp:
            build_dir = self._build_dir(tmp)
            self._write_baseline(tmp, "esp32", text=1000, data=0, bss=30, flash_total=1000)
            # +20 on a base of 1000 -> exactly 2.00% == default threshold
            result = {
                "elf": str(build_dir / "firmware.elf"), "build_dir": str(build_dir),
                "text": 1020, "data": 0, "bss": 30, "flash_total": 1020, "components": {},
            }
            with self._fake_measure(result):
                out_buf = io.StringIO()
                with contextlib.redirect_stdout(out_buf):
                    rc = run(_args(str(build_dir), root=tmp, target="esp32", check=True))
            self.assertEqual(rc, 0)
            self.assertIn("PASS", out_buf.getvalue())


_HEAP_RESPONSE = {
    "internal": {
        "free": 150000,
        "allocated": 50000,
        "largest_free_block": 90000,
        "minimum_ever_free": 120000,
    },
    "dma": {
        "free": 40000,
        "allocated": 10000,
        "largest_free_block": 20000,
        "minimum_ever_free": 35000,
    },
}


class _FakeHttpResponse:
    """Minimal context-manager stand-in for urllib.request.urlopen()'s
    return value -- exposes .read() bytes, used to mock the heap endpoint
    without any real network access."""

    def __init__(self, body: bytes):
        self._body = body

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False

    def read(self):
        return self._body


def _fake_urlopen(body_dict):
    return mock.patch(
        "commands.size.urllib.request.urlopen",
        return_value=_FakeHttpResponse(json.dumps(body_dict).encode("utf-8")),
    )


class TestHeapHttpCapture(unittest.TestCase):
    def _fake_measure(self, result):
        return mock.patch(
            "commands.size._measure",
            return_value=(result, None, None),
        )

    def _result(self, build_dir):
        return {
            "elf": str(build_dir / "firmware.elf"), "build_dir": str(build_dir),
            "text": 100, "data": 20, "bss": 30, "flash_total": 120,
            "components": {"bb_core": 50},
        }

    def test_update_baseline_with_heap_from_http_writes_heap_block(self):
        with tempfile.TemporaryDirectory() as tmp:
            build_dir = Path(tmp, "examples", "smoke", ".pio", "build", "esp32")
            build_dir.mkdir(parents=True)
            Path(build_dir, "firmware.elf").write_bytes(b"\x7fELF")
            with self._fake_measure(self._result(build_dir)), _fake_urlopen(_HEAP_RESPONSE):
                rc = run(_args(
                    str(build_dir), root=tmp, target="esp32", update_baseline=True,
                    heap_from_http="http://example-device.test",
                ))
            self.assertEqual(rc, 0)
            baseline = json.loads(baseline_path(tmp, "esp32").read_text(encoding="utf-8"))
            heap = baseline["heap"]
            self.assertEqual(heap["source"], "http")
            self.assertEqual(heap["min_free"], 120000)
            self.assertEqual(heap["high_water"], 120000)
            self.assertEqual(heap["free"], 150000)
            self.assertEqual(heap["largest_free_block"], 90000)
            self.assertEqual(heap["regions"]["internal"]["min_free"], 120000)
            self.assertEqual(heap["regions"]["dma"]["min_free"], 35000)
            # flash + config are preserved, not clobbered by the heap capture
            self.assertEqual(baseline["flash"]["flash_total"], 120)
            self.assertIn("sdkconfig_sha", baseline["config"])

    def test_update_baseline_heap_http_error_returns_clean_rc1(self):
        with tempfile.TemporaryDirectory() as tmp:
            build_dir = Path(tmp, "examples", "smoke", ".pio", "build", "esp32")
            build_dir.mkdir(parents=True)
            Path(build_dir, "firmware.elf").write_bytes(b"\x7fELF")
            with self._fake_measure(self._result(build_dir)), \
                 mock.patch(
                     "commands.size.urllib.request.urlopen",
                     side_effect=OSError("connection refused"),
                 ):
                buf = io.StringIO()
                with contextlib.redirect_stderr(buf):
                    rc = run(_args(
                        str(build_dir), root=tmp, target="esp32", update_baseline=True,
                        heap_from_http="http://unreachable.test",
                    ))
            self.assertEqual(rc, 1)
            self.assertIn("bbtool size: error:", buf.getvalue())
            self.assertNotIn("Traceback", buf.getvalue())
            self.assertFalse(baseline_path(tmp, "esp32").is_file())

    def test_update_baseline_heap_http_bad_json_returns_clean_rc1(self):
        with tempfile.TemporaryDirectory() as tmp:
            build_dir = Path(tmp, "examples", "smoke", ".pio", "build", "esp32")
            build_dir.mkdir(parents=True)
            Path(build_dir, "firmware.elf").write_bytes(b"\x7fELF")
            with self._fake_measure(self._result(build_dir)), \
                 mock.patch(
                     "commands.size.urllib.request.urlopen",
                     return_value=_FakeHttpResponse(b"not json"),
                 ):
                buf = io.StringIO()
                with contextlib.redirect_stderr(buf):
                    rc = run(_args(
                        str(build_dir), root=tmp, target="esp32", update_baseline=True,
                        heap_from_http="http://example-device.test",
                    ))
            self.assertEqual(rc, 1)
            self.assertIn("bbtool size: error:", buf.getvalue())
            self.assertNotIn("Traceback", buf.getvalue())

    def test_update_baseline_heap_http_missing_internal_returns_clean_rc1(self):
        with tempfile.TemporaryDirectory() as tmp:
            build_dir = Path(tmp, "examples", "smoke", ".pio", "build", "esp32")
            build_dir.mkdir(parents=True)
            Path(build_dir, "firmware.elf").write_bytes(b"\x7fELF")
            with self._fake_measure(self._result(build_dir)), _fake_urlopen({"dma": {"free": 1}}):
                buf = io.StringIO()
                with contextlib.redirect_stderr(buf):
                    rc = run(_args(
                        str(build_dir), root=tmp, target="esp32", update_baseline=True,
                        heap_from_http="http://example-device.test",
                    ))
            self.assertEqual(rc, 1)
            self.assertIn("missing 'internal'", buf.getvalue())

    def test_update_baseline_heap_http_urlerror_returns_clean_rc1(self):
        with tempfile.TemporaryDirectory() as tmp:
            build_dir = Path(tmp, "examples", "smoke", ".pio", "build", "esp32")
            build_dir.mkdir(parents=True)
            Path(build_dir, "firmware.elf").write_bytes(b"\x7fELF")
            with self._fake_measure(self._result(build_dir)), \
                 mock.patch(
                     "commands.size.urllib.request.urlopen",
                     side_effect=urllib.error.URLError("name resolution failed"),
                 ):
                buf = io.StringIO()
                with contextlib.redirect_stderr(buf):
                    rc = run(_args(
                        str(build_dir), root=tmp, target="esp32", update_baseline=True,
                        heap_from_http="http://unreachable.test",
                    ))
            self.assertEqual(rc, 1)
            self.assertIn("bbtool size: error:", buf.getvalue())
            self.assertNotIn("Traceback", buf.getvalue())
            self.assertFalse(baseline_path(tmp, "esp32").is_file())

    def test_check_heap_from_http_httperror_returns_clean_rc1(self):
        with tempfile.TemporaryDirectory() as tmp:
            build_dir = Path(tmp, "examples", "smoke", ".pio", "build", "esp32")
            build_dir.mkdir(parents=True)
            Path(build_dir, "firmware.elf").write_bytes(b"\x7fELF")
            payload = {
                "target": "esp32", "arch": "xtensa",
                "config": {"label": "default", "toolchain": "esp-idf", "sdkconfig_sha": "x", "snapshot": "x"},
                "flash": {"text": 100, "data": 20, "bss": 30, "flash_total": 120, "components": {}},
                "heap": {"min_free": None, "high_water": None, "regions": None, "source": None},
            }
            path = baseline_path(tmp, "esp32")
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(json.dumps(payload), encoding="utf-8")
            with self._fake_measure(self._result(build_dir)), \
                 mock.patch(
                     "commands.size.urllib.request.urlopen",
                     side_effect=urllib.error.HTTPError(
                         "http://example-device.test/api/diag/heap", 503, "Service Unavailable", {}, None,
                     ),
                 ):
                buf = io.StringIO()
                with contextlib.redirect_stderr(buf):
                    rc = run(_args(
                        str(build_dir), root=tmp, target="esp32", check=True,
                        heap_from_http="http://example-device.test",
                    ))
            self.assertEqual(rc, 1)
            self.assertIn("bbtool size: error:", buf.getvalue())
            self.assertNotIn("Traceback", buf.getvalue())

    def test_check_heap_from_http_timeout_returns_clean_rc1(self):
        with tempfile.TemporaryDirectory() as tmp:
            build_dir = Path(tmp, "examples", "smoke", ".pio", "build", "esp32")
            build_dir.mkdir(parents=True)
            Path(build_dir, "firmware.elf").write_bytes(b"\x7fELF")
            payload = {
                "target": "esp32", "arch": "xtensa",
                "config": {"label": "default", "toolchain": "esp-idf", "sdkconfig_sha": "x", "snapshot": "x"},
                "flash": {"text": 100, "data": 20, "bss": 30, "flash_total": 120, "components": {}},
                "heap": {"min_free": None, "high_water": None, "regions": None, "source": None},
            }
            path = baseline_path(tmp, "esp32")
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(json.dumps(payload), encoding="utf-8")
            with self._fake_measure(self._result(build_dir)), \
                 mock.patch(
                     "commands.size.urllib.request.urlopen",
                     side_effect=TimeoutError("timed out"),
                 ):
                buf = io.StringIO()
                with contextlib.redirect_stderr(buf):
                    rc = run(_args(
                        str(build_dir), root=tmp, target="esp32", check=True,
                        heap_from_http="http://example-device.test",
                    ))
            self.assertEqual(rc, 1)
            self.assertIn("bbtool size: error:", buf.getvalue())
            self.assertNotIn("Traceback", buf.getvalue())


class TestCompareHeap(unittest.TestCase):
    def test_inert_when_baseline_heap_null(self):
        ok = size_mod._compare_heap("esp32", {"min_free": 100}, None)
        self.assertTrue(ok)
        ok = size_mod._compare_heap(
            "esp32", {"min_free": 100},
            {"min_free": None, "high_water": None, "regions": None, "source": None},
        )
        self.assertTrue(ok)

    def test_min_free_regression_fails(self):
        baseline = {"min_free": 120000, "high_water": 120000, "regions": {}, "source": "http"}
        current = {"min_free": 90000, "high_water": 90000, "regions": {}}
        buf = io.StringIO()
        with contextlib.redirect_stderr(buf):
            ok = size_mod._compare_heap("esp32", current, baseline)
        self.assertFalse(ok)
        self.assertIn("min_free regressed", buf.getvalue())

    def test_min_free_equal_or_higher_passes(self):
        baseline = {"min_free": 120000, "high_water": 120000, "regions": {}, "source": "http"}
        current = {"min_free": 120000, "high_water": 130000, "regions": {}}
        out_buf = io.StringIO()
        with contextlib.redirect_stdout(out_buf):
            ok = size_mod._compare_heap("esp32", current, baseline)
        self.assertTrue(ok)
        self.assertIn("heap PASS", out_buf.getvalue())

    def test_region_min_free_regression_fails(self):
        baseline = {
            "min_free": 120000, "high_water": 120000,
            "regions": {"dma": {"min_free": 35000}}, "source": "http",
        }
        current = {
            "min_free": 120000, "high_water": 120000,
            "regions": {"dma": {"min_free": 30000}},
        }
        buf = io.StringIO()
        with contextlib.redirect_stderr(buf):
            ok = size_mod._compare_heap("esp32", current, baseline)
        self.assertFalse(ok)
        self.assertIn("regions.dma.min_free regressed", buf.getvalue())


class TestExtractHeap(unittest.TestCase):
    def test_extracts_min_free_and_regions(self):
        heap, error = size_mod._extract_heap(_HEAP_RESPONSE)
        self.assertIsNone(error)
        self.assertEqual(heap["min_free"], 120000)
        self.assertEqual(heap["high_water"], 120000)
        self.assertEqual(set(heap["regions"]), {"internal", "dma"})
        self.assertEqual(heap["source"], "http")

    def test_missing_internal_capability_errors(self):
        heap, error = size_mod._extract_heap({"dma": {"free": 1}})
        self.assertIsNone(heap)
        self.assertIn("missing 'internal'", error)

    def test_ignores_non_dict_top_level_keys(self):
        raw = dict(_HEAP_RESPONSE)
        raw["integrity_ok"] = True
        heap, error = size_mod._extract_heap(raw)
        self.assertIsNone(error)
        self.assertNotIn("integrity_ok", heap["regions"])


if __name__ == "__main__":
    unittest.main()
