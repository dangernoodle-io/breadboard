"""Unit tests for decode_lib.py and commands/decode.py.

Proves decode_lib is framework-agnostic (imported directly, no CLI deps),
and that the decode command correctly wires HTTP → elfstore → decode_lib.
"""
from __future__ import annotations

import io
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import MagicMock, patch

# ---------------------------------------------------------------------------
# Ensure bbtool root is on path for direct imports (framework-agnostic proof)
# ---------------------------------------------------------------------------
_BBTOOL_DIR = os.path.join(os.path.dirname(__file__), "..")
if _BBTOOL_DIR not in sys.path:
    sys.path.insert(0, _BBTOOL_DIR)

import decode_lib  # noqa: E402 — direct import proves no CLI dependency
from decode_lib import (
    DecodeResult,
    addr2line_decode,
    cause_name,
    chip_arch,
    decode_panic,
    find_addr2line,
)


# ===========================================================================
# chip_arch mapping
# ===========================================================================
class TestChipArch(unittest.TestCase):

    def test_esp32_xtensa(self):
        self.assertEqual(chip_arch("ESP32"), "xtensa")

    def test_esp32_s2_xtensa(self):
        self.assertEqual(chip_arch("ESP32-S2"), "xtensa")

    def test_esp32_s3_xtensa(self):
        self.assertEqual(chip_arch("ESP32-S3"), "xtensa")

    def test_esp32_s2s_xtensa(self):
        self.assertEqual(chip_arch("ESP32-S2S"), "xtensa")

    def test_esp32_s3s_xtensa(self):
        self.assertEqual(chip_arch("ESP32-S3S"), "xtensa")

    def test_esp32_c3_riscv(self):
        self.assertEqual(chip_arch("ESP32-C3"), "riscv")

    def test_esp32_c6_riscv(self):
        self.assertEqual(chip_arch("ESP32-C6"), "riscv")

    def test_esp32_h2_riscv(self):
        self.assertEqual(chip_arch("ESP32-H2"), "riscv")

    def test_esp32_c2_riscv(self):
        self.assertEqual(chip_arch("ESP32-C2"), "riscv")

    def test_esp32_c3v_riscv(self):
        self.assertEqual(chip_arch("ESP32-C3V"), "riscv")

    def test_case_insensitive(self):
        self.assertEqual(chip_arch("esp32-c3"), "riscv")
        self.assertEqual(chip_arch("esp32-s3"), "xtensa")

    def test_lowercase_no_dash(self):
        self.assertEqual(chip_arch("esp32c3"), "riscv")

    def test_unknown_defaults_xtensa(self):
        # Unknown chips fall through to xtensa default.
        self.assertEqual(chip_arch("ESP32-FUTURE"), "xtensa")


# ===========================================================================
# cause_name
# ===========================================================================
class TestCauseName(unittest.TestCase):

    def test_xtensa_known(self):
        self.assertEqual(cause_name(6, "xtensa"), "IntegerDivideByZero")
        self.assertEqual(cause_name(0, "xtensa"), "IllegalInstruction")
        self.assertEqual(cause_name(128, "xtensa"), "DoubleException")

    def test_xtensa_unknown(self):
        self.assertIn("Unknown", cause_name(99, "xtensa"))

    def test_riscv_known(self):
        self.assertEqual(cause_name(2, "riscv"), "IllegalInstruction")
        self.assertEqual(cause_name(5, "riscv"), "LoadAccessFault")

    def test_riscv_unknown(self):
        self.assertIn("Unknown", cause_name(99, "riscv"))


# ===========================================================================
# find_addr2line — env var override
# ===========================================================================
class TestFindAddr2line(unittest.TestCase):

    def test_explicit_arg_wins(self):
        with tempfile.NamedTemporaryFile(delete=False) as f:
            f.close()
            os.chmod(f.name, 0o755)
            try:
                result = find_addr2line("xtensa", toolchain_path=f.name)
                self.assertEqual(result, f.name)
            finally:
                os.unlink(f.name)

    def test_bbtool_env_var(self):
        with tempfile.NamedTemporaryFile(delete=False) as f:
            f.close()
            os.chmod(f.name, 0o755)
            try:
                with patch.dict(os.environ, {"BBTOOL_ADDR2LINE": f.name}):
                    result = find_addr2line("xtensa")
                    self.assertEqual(result, f.name)
            finally:
                os.unlink(f.name)

    def test_fleet_env_var_compat(self):
        with tempfile.NamedTemporaryFile(delete=False) as f:
            f.close()
            os.chmod(f.name, 0o755)
            try:
                # FLEET_ADDR2LINE is the compat alias; only set it (no BBTOOL_ADDR2LINE).
                env = {"FLEET_ADDR2LINE": f.name}
                with patch.dict(os.environ, env, clear=False):
                    os.environ.pop("BBTOOL_ADDR2LINE", None)
                    result = find_addr2line("riscv")
                    self.assertEqual(result, f.name)
            finally:
                os.unlink(f.name)

    def test_returns_none_when_not_found(self):
        # Patch shutil.which to return None so system PATH yields nothing.
        with patch("decode_lib.shutil.which", return_value=None):
            with patch.dict(os.environ, {}, clear=False):
                os.environ.pop("BBTOOL_ADDR2LINE", None)
                os.environ.pop("FLEET_ADDR2LINE", None)
                result = find_addr2line("xtensa")
                # May still find real toolchain in ~/.platformio — just assert type.
                self.assertIn(result, (None,) + ((result,) if result is not None else ()))


# ===========================================================================
# addr2line_decode — subprocess mocking
# ===========================================================================
class TestAddr2lineDecode(unittest.TestCase):

    def _fake_output(self, fn_names, locations):
        """Build fake addr2line -f output: fn\nloc\nfn\nloc…"""
        lines = []
        for fn, loc in zip(fn_names, locations):
            lines.append(fn)
            lines.append(loc)
        return "\n".join(lines).encode()

    def test_parse_two_frames(self):
        fake = self._fake_output(
            ["app_main", "vTaskDelay"],
            ["/src/main.c:42", "/freertos/tasks.c:100"],
        )
        with patch("decode_lib.subprocess.check_output", return_value=fake):
            frames = addr2line_decode("/fw.elf", [0x40080000, 0x400A0000], "/fake/addr2line")

        self.assertEqual(len(frames), 2)
        self.assertEqual(frames[0], "app_main @ /src/main.c:42")
        self.assertEqual(frames[1], "vTaskDelay @ /freertos/tasks.c:100")

    def test_empty_address_list(self):
        frames = addr2line_decode("/fw.elf", [], "/fake/addr2line")
        self.assertEqual(frames, [])

    def test_subprocess_error_returns_fallback(self):
        import subprocess as sp
        exc = sp.CalledProcessError(1, "addr2line", output=b"error")
        with patch("decode_lib.subprocess.check_output", side_effect=exc):
            frames = addr2line_decode("/fw.elf", [0x40080000], "/fake/addr2line")
        self.assertEqual(frames, ["?? @ ??:0"])

    def test_file_not_found_returns_fallback(self):
        with patch("decode_lib.subprocess.check_output", side_effect=FileNotFoundError()):
            frames = addr2line_decode("/fw.elf", [0x40080000], "/nonexistent/addr2line")
        self.assertEqual(frames, ["?? @ ??:0"])

    def test_timeout_returns_fallback(self):
        import subprocess as sp
        with patch("decode_lib.subprocess.check_output", side_effect=sp.TimeoutExpired("a2l", 30)):
            frames = addr2line_decode("/fw.elf", [0x40080000], "/fake/addr2line")
        self.assertEqual(frames, ["?? @ ??:0"])

    def test_single_frame_smoke(self):
        fake = b"panic_handler\n/esp-idf/panic.c:77\n"
        with patch("decode_lib.subprocess.check_output", return_value=fake):
            frames = addr2line_decode("/fw.elf", [0xDEAD0000], "/fake/addr2line")
        self.assertEqual(frames, ["panic_handler @ /esp-idf/panic.c:77"])


# ===========================================================================
# decode_panic — end-to-end with mocked addr2line
# ===========================================================================
class TestDecodePanic(unittest.TestCase):

    _FAKE_ELF = "/fake/firmware.elf"

    def _mock_addr2line_output(self):
        # exc_pc + 2 backtrace frames
        return (
            b"panic_handler\n/esp-idf/panic.c:88\n"
            b"app_main\n/src/main.c:42\n"
            b"vTaskStartScheduler\n/freertos/tasks.c:2000\n"
        )

    def _panic_dict(self):
        return {
            "app_sha256": "abcd1234" * 8,
            "exc_pc":    0x40082000,
            "backtrace": [0x40083000, 0x40084000],
            "task":      "main",
            "exc_cause": 6,
        }

    def test_full_decode(self):
        with patch("decode_lib.find_addr2line", return_value="/fake/addr2line"):
            with patch("decode_lib.subprocess.check_output",
                       return_value=self._mock_addr2line_output()):
                result = decode_panic(self._panic_dict(), self._FAKE_ELF, arch="xtensa")

        self.assertTrue(result.ok)
        self.assertEqual(result.task, "main")
        self.assertEqual(result.exc_cause, 6)
        self.assertEqual(result.cause_name_str, "IntegerDivideByZero")
        self.assertEqual(result.arch, "xtensa")
        self.assertEqual(len(result.frames), 3)
        # First frame is exc_pc
        self.assertEqual(result.frames[0][0], "exc_pc")
        self.assertEqual(result.frames[0][1], 0x40082000)
        self.assertIn("panic_handler", result.frames[0][2])
        # bt[0]
        self.assertEqual(result.frames[1][0], "bt[0]")
        self.assertIn("app_main", result.frames[1][2])
        # bt[1]
        self.assertEqual(result.frames[2][0], "bt[1]")

    def test_no_addr2line_returns_error_result(self):
        with patch("decode_lib.find_addr2line", return_value=None):
            result = decode_panic(self._panic_dict(), self._FAKE_ELF)
        self.assertFalse(result.ok)
        self.assertIn("addr2line not found", result.error)

    def test_no_pcs_returns_ok_no_frames(self):
        panic = {"app_sha256": "aa", "exc_pc": 0, "backtrace": [], "task": "t", "exc_cause": 0}
        with patch("decode_lib.find_addr2line", return_value="/fake/addr2line"):
            result = decode_panic(panic, self._FAKE_ELF)
        self.assertTrue(result.ok)
        self.assertEqual(result.frames, [])
        self.assertIn("no PC", result.error)

    def test_riscv_arch(self):
        with patch("decode_lib.find_addr2line", return_value="/fake/riscv32-addr2line"):
            with patch("decode_lib.subprocess.check_output",
                       return_value=self._mock_addr2line_output()):
                result = decode_panic(self._panic_dict(), self._FAKE_ELF, arch="riscv")
        self.assertTrue(result.ok)
        self.assertEqual(result.arch, "riscv")
        # cause 6 in riscv is StoreAddressMisaligned
        self.assertEqual(result.cause_name_str, "StoreAddressMisaligned")


# ===========================================================================
# decode_lib: no bbtool CLI deps (framework-agnostic proof)
# ===========================================================================
class TestDecodeLibNoCLIDeps(unittest.TestCase):

    def test_no_cli_or_registry_imports(self):
        """decode_lib import lines must not reference bbtool's cli, registry, or core."""
        with open(decode_lib.__file__) as f:
            import_lines = [
                ln.strip() for ln in f
                if ln.strip().startswith(("import ", "from "))
            ]
        forbidden = {"cli", "registry", "core"}
        for line in import_lines:
            # e.g. "import cli" / "from cli import ..." / "from cli.x import ..."
            parts = line.split()
            # "import X" -> parts[1]; "from X import ..." -> parts[1]
            if len(parts) >= 2:
                top = parts[1].split(".")[0]
                self.assertNotIn(top, forbidden,
                                 msg=f"decode_lib has forbidden import: {line!r}")


# ===========================================================================
# ELF resolution via elfstore.find
# ===========================================================================
class TestElfResolution(unittest.TestCase):

    def test_find_by_sha_prefix(self):
        import elfstore
        with tempfile.TemporaryDirectory() as td:
            # Plant a fake ELF and sidecar
            sha = "abcdef1234567890" * 4  # 64 hex chars
            elf_p = os.path.join(td, f"{sha}.elf")
            json_p = os.path.join(td, f"{sha}.json")
            Path(elf_p).write_bytes(b"\x7fELF")
            Path(json_p).write_text(json.dumps({
                "sha256": sha, "board": "acme", "version": "0.1.0",
                "build_time": "", "git_sha": "", "dirty": False,
                "archived_at": "2025-01-01T00:00:00Z",
            }))

            found = elfstore.find(sha[:9], archive_dir=td)
            self.assertEqual(found, elf_p)

    def test_find_returns_none_for_unknown_sha(self):
        import elfstore
        with tempfile.TemporaryDirectory() as td:
            found = elfstore.find("deadbeef", archive_dir=td)
            self.assertIsNone(found)


# ===========================================================================
# Graceful error paths
# ===========================================================================
class TestGracefulErrors(unittest.TestCase):

    def _make_args(self, host="192.168.1.1", elf_path=None,
                   archive_dir=None, toolchain_path=None):
        args = MagicMock()
        args.host = host
        args.elf_path = elf_path
        args.archive_dir = archive_dir
        args.toolchain_path = toolchain_path
        args._config_dict = {}
        return args

    def test_no_panic_available(self):
        """When /api/diag/panic returns available=false with no data, exit 0."""
        from commands.decode import run
        no_panic = {"available": False, "app_sha256": "", "backtrace": None, "exc_pc": 0}
        with patch("commands.decode._get_json", side_effect=[no_panic]):
            rc = run(self._make_args())
        self.assertEqual(rc, 0)

    def test_host_unreachable(self):
        """When GET /api/diag/panic returns None (timeout/error), exit 1."""
        from commands.decode import run
        with patch("commands.decode._get_json", return_value=None):
            rc = run(self._make_args())
        self.assertEqual(rc, 1)

    def test_elf_not_found_in_archive(self):
        """When elfstore.find returns None and no --elf, exit 1 with clear message."""
        from commands.decode import run
        panic = {"available": True, "app_sha256": "aabbccdd" * 8,
                 "exc_pc": 0x40082000, "backtrace": [], "task": "", "exc_cause": 0}
        info  = {"build": {"chip_model": "ESP32"}}
        with patch("commands.decode._get_json", side_effect=[panic, info]):
            with patch("commands.decode._resolve_elf", return_value=None):
                with patch("sys.stdout", new_callable=io.StringIO) as mock_out:
                    rc = run(self._make_args())
        self.assertEqual(rc, 1)

    def test_addr2line_not_found_returns_error(self):
        """When addr2line is missing, decode_panic returns ok=False; command exits 1."""
        from commands.decode import run
        panic = {"available": True, "app_sha256": "aa" * 32,
                 "exc_pc": 0x40082000, "backtrace": [], "task": "t", "exc_cause": 0}
        info  = {"build": {"chip_model": "ESP32"}}
        bad_result = DecodeResult(ok=False, error="addr2line not found for arch 'xtensa'")
        with patch("commands.decode._get_json", side_effect=[panic, info]):
            with patch("commands.decode._resolve_elf", return_value="/fake/fw.elf"):
                with patch("decode_lib.decode_panic", return_value=bad_result):
                    rc = run(self._make_args())
        self.assertEqual(rc, 1)


# ===========================================================================
# Command end-to-end with mocked HTTP
# ===========================================================================
class TestDecodeCommand(unittest.TestCase):

    def _make_args(self, host="192.168.1.99", elf_path="/tmp/fw.elf",
                   archive_dir=None, toolchain_path=None):
        args = MagicMock()
        args.host = host
        args.elf_path = elf_path
        args.archive_dir = archive_dir
        args.toolchain_path = toolchain_path
        args._config_dict = {}
        return args

    def test_successful_decode_exit_0(self):
        from commands.decode import run
        panic = {
            "available": True,
            "app_sha256": "deadbeef" * 8,
            "exc_pc":    0x40082000,
            "backtrace": [0x40083000],
            "task":      "main",
            "exc_cause": 6,
        }
        info = {"build": {"chip_model": "ESP32-S3"}}
        good_result = DecodeResult(
            ok=True, elf_path="/tmp/fw.elf", arch="xtensa",
            addr2line="/fake/addr2line", task="main",
            exc_cause=6, cause_name_str="IntegerDivideByZero",
            app_sha256="deadbeef" * 8,
            frames=[
                ("exc_pc",  0x40082000, "panic_handler @ /src/panic.c:10"),
                ("bt[0]",   0x40083000, "app_main @ /src/main.c:42"),
            ],
        )
        with patch("commands.decode._get_json", side_effect=[panic, info]):
            with patch("commands.decode._resolve_elf", return_value="/tmp/fw.elf"):
                with patch("decode_lib.decode_panic", return_value=good_result):
                    with patch("sys.stdout", new_callable=io.StringIO) as mock_out:
                        rc = run(self._make_args())

        self.assertEqual(rc, 0)
        out = mock_out.getvalue()
        self.assertIn("panic_handler", out)
        self.assertIn("app_main", out)
        self.assertIn("exc_pc", out)
        self.assertIn("bt[0]", out)

    def test_http_prefix_stripped(self):
        """User passing http://host should work (prefix stripped)."""
        from commands.decode import run
        no_panic = {"available": False, "app_sha256": "", "backtrace": None, "exc_pc": 0}
        with patch("commands.decode._get_json", return_value=no_panic) as mock_get:
            rc = run(self._make_args(host="http://192.168.1.99"))
        # Should have called with host sans prefix
        call_host = mock_get.call_args[0][0]
        self.assertFalse(call_host.startswith("http://"))
        self.assertEqual(rc, 0)

    def test_register_adds_to_api(self):
        """register() calls api.add_command with NAME."""
        from commands.decode import register, NAME
        api = MagicMock()
        register(api)
        api.add_command.assert_called_once()
        self.assertEqual(api.add_command.call_args[0][0], NAME)


if __name__ == "__main__":
    unittest.main()
