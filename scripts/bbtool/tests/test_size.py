"""size command tests: pure `size`-output + .map-file parsing logic (no
toolchain/hardware required), plus the `run()` CLI entry point exercised via
mocked subprocess/toolchain-discovery calls."""
import argparse
import contextlib
import io
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "commands"))

from commands.size import parse_map_component_sizes, parse_size_output, run


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


if __name__ == "__main__":
    unittest.main()
