"""Unit tests for ota_server.py — uses OtaTestServer embeddable handle."""
from __future__ import annotations

import http.client
import json
import os
import pathlib
import ssl
import struct
import sys
import tempfile
import unittest

_BBTOOL_DIR = os.path.join(os.path.dirname(__file__), "..")
if _BBTOOL_DIR not in sys.path:
    sys.path.insert(0, _BBTOOL_DIR)

import ota_server
from ota_server import (
    OtaTestServer,
    compute_tag,
    discover_bins,
    parse_app_desc_from_bin,
)


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

_MAGIC = 0xABCD5432


def make_fake_bin(project_name: str = "myboard", version: str = "v1.0.0", prefix_size: int = 32) -> bytes:
    """Build a minimal .bin with esp_app_desc_t magic at prefix_size offset."""
    magic_bytes = struct.pack("<I", _MAGIC)
    secure_version = struct.pack("<I", 0)
    reserv1 = b"\x00" * 8
    ver_bytes = version.encode().ljust(32, b"\x00")[:32]
    name_bytes = project_name.encode().ljust(32, b"\x00")[:32]
    prefix = b"\x00" * prefix_size
    trailer = b"\x00" * 100
    return prefix + magic_bytes + secure_version + reserv1 + ver_bytes + name_bytes + trailer


def write_fake_bin(directory: str, filename: str, project_name: str = "myboard",
                   version: str = "v1.0.0", prefix_size: int = 32) -> str:
    path = os.path.join(directory, filename)
    pathlib.Path(path).write_bytes(make_fake_bin(project_name, version, prefix_size))
    return path


def http_server(tmp_path, boards=None, use_http=True, **kwargs):
    """Start a test OtaTestServer and return it as a context manager."""
    p = pathlib.Path(tmp_path)
    if boards is None:
        boards = [("myboard", "v1.0.0")]
    for board, version in boards:
        write_fake_bin(str(p), f"{board}.bin", board, version)
    return OtaTestServer(str(p), use_http=use_http, **kwargs)


# ---------------------------------------------------------------------------
# parse_app_desc_from_bin
# ---------------------------------------------------------------------------

class TestParseAppDesc(unittest.TestCase):

    def test_parse_app_desc_basic(self):
        with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as f:
            f.write(make_fake_bin("myboard", "v1.2.3"))
            path = f.name
        try:
            name, ver = parse_app_desc_from_bin(path)
            self.assertEqual(name, "myboard")
            self.assertEqual(ver, "v1.2.3")
        finally:
            os.unlink(path)

    def test_parse_app_desc_different_offset(self):
        with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as f:
            f.write(make_fake_bin("boardx", "v2.0.1", prefix_size=64))
            path = f.name
        try:
            name, ver = parse_app_desc_from_bin(path)
            self.assertEqual(name, "boardx")
            self.assertEqual(ver, "v2.0.1")
        finally:
            os.unlink(path)

    def test_parse_app_desc_no_magic_raises(self):
        with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as f:
            f.write(b"\x00" * 256)
            path = f.name
        try:
            with self.assertRaises(ValueError):
                parse_app_desc_from_bin(path)
        finally:
            os.unlink(path)


# ---------------------------------------------------------------------------
# discover_bins
# ---------------------------------------------------------------------------

class TestDiscoverBins(unittest.TestCase):

    def test_discover_two_boards(self):
        with tempfile.TemporaryDirectory() as td:
            write_fake_bin(td, "fw-alpha.bin", "alpha", "v1.0.0")
            write_fake_bin(td, "fw-beta.bin", "beta", "v2.0.0")
            result = discover_bins(td)
        self.assertIn("alpha", result)
        self.assertIn("beta", result)
        self.assertEqual(result["alpha"][1], "v1.0.0")
        self.assertEqual(result["beta"][1], "v2.0.0")

    def test_discover_skips_invalid(self):
        with tempfile.TemporaryDirectory() as td:
            pathlib.Path(td, "bad.bin").write_bytes(b"\x00" * 64)
            write_fake_bin(td, "good.bin", "good", "v1.0.0")
            result = discover_bins(td)
        self.assertNotIn("bad", result)
        self.assertIn("good", result)


# ---------------------------------------------------------------------------
# compute_tag
# ---------------------------------------------------------------------------

class TestComputeTag(unittest.TestCase):

    def test_bump_patch(self):
        self.assertEqual(compute_tag(["v1.2.3", "v1.0.0"]), "v1.2.4")

    def test_explicit_tag(self):
        self.assertEqual(compute_tag(["v1.2.3"], tag="v5.0.0"), "v5.0.0")

    def test_no_bump(self):
        result = compute_tag(["v1.2.3", "v1.0.0"], no_bump=True)
        self.assertEqual(result, "v1.2.3")

    def test_no_bump_no_versions(self):
        self.assertEqual(compute_tag([], no_bump=True), "v0.0.0")

    def test_sentinel_no_versions(self):
        self.assertEqual(compute_tag([]), "v99.99.99")

    def test_dev_builds_treated_as_zero(self):
        # dev build should parse as 0.0.0, so bumping gives v0.0.1
        result = compute_tag(["devbuild"])
        self.assertEqual(result, "v0.0.1")

    def test_mixed_dev_and_real(self):
        result = compute_tag(["devbuild", "v1.0.0"])
        self.assertEqual(result, "v1.0.1")


# ---------------------------------------------------------------------------
# GitHub provider manifest shape
# ---------------------------------------------------------------------------

class TestGitHubManifest(unittest.TestCase):

    def test_manifest_shape(self):
        p = ota_server.GitHubProvider()
        board_map = {"myboard": ("/fake/myboard.bin", "v1.0.0")}
        m = p.manifest(board_map, tag="v1.0.1", advertise_base="http://127.0.0.1:8070")
        self.assertEqual(m["tag_name"], "v1.0.1")
        assets = m["assets"]
        self.assertEqual(len(assets), 1)
        self.assertEqual(assets[0]["name"], "myboard.bin")
        self.assertIn("myboard.bin", assets[0]["browser_download_url"])

    def test_asset_path_format(self):
        p = ota_server.GitHubProvider()
        self.assertEqual(p.asset_path("myboard", "v1.0.1"),
                         "/local/firmware/releases/download/v1.0.1/myboard.bin")

    def test_cdn_path_format(self):
        p = ota_server.GitHubProvider()
        cdn = p.cdn_path("myboard")
        self.assertTrue(cdn.startswith("/cdn/"))
        self.assertTrue(cdn.endswith("/myboard.bin"))


# ---------------------------------------------------------------------------
# HTTP server tests (plain HTTP via OtaTestServer context manager)
# ---------------------------------------------------------------------------

class TestHTTPServer(unittest.TestCase):

    def _conn(self, srv):
        return http.client.HTTPConnection("127.0.0.1", srv.port, timeout=5)

    def test_manifest_200(self):
        with tempfile.TemporaryDirectory() as td:
            with http_server(td, advertise_host="127.0.0.1") as srv:
                conn = self._conn(srv)
                conn.request("GET", "/releases/latest")
                resp = conn.getresponse()
                self.assertEqual(resp.status, 200)
                data = json.loads(resp.read())
                self.assertIn("tag_name", data)
                self.assertIn("assets", data)
                conn.close()

    def test_redirect_302(self):
        with tempfile.TemporaryDirectory() as td:
            with http_server(td, advertise_host="127.0.0.1") as srv:
                tag = srv.tag
                conn = self._conn(srv)
                conn.request("GET", f"/local/firmware/releases/download/{tag}/myboard.bin")
                resp = conn.getresponse()
                resp.read()
                self.assertEqual(resp.status, 302)
                loc = resp.getheader("Location")
                self.assertIsNotNone(loc)
                self.assertIn("/cdn/", loc)
                conn.close()

    def test_cdn_head_403(self):
        with tempfile.TemporaryDirectory() as td:
            with http_server(td, advertise_host="127.0.0.1") as srv:
                provider = srv._server.provider
                cdn_path = provider.cdn_path("myboard")
                conn = self._conn(srv)
                conn.request("HEAD", cdn_path)
                resp = conn.getresponse()
                resp.read()
                self.assertEqual(resp.status, 403)
                conn.close()

    def test_cdn_head_ok(self):
        with tempfile.TemporaryDirectory() as td:
            with http_server(td, advertise_host="127.0.0.1", head_ok=True) as srv:
                provider = srv._server.provider
                cdn_path = provider.cdn_path("myboard")
                conn = self._conn(srv)
                conn.request("HEAD", cdn_path)
                resp = conn.getresponse()
                resp.read()
                self.assertEqual(resp.status, 200)
                conn.close()

    def test_cdn_get_full(self):
        with tempfile.TemporaryDirectory() as td:
            src_data = make_fake_bin("myboard", "v1.0.0")
            pathlib.Path(td, "myboard.bin").write_bytes(src_data)
            with OtaTestServer(td, use_http=True, advertise_host="127.0.0.1").start() as srv:
                provider = srv._server.provider
                cdn_path = provider.cdn_path("myboard")
                conn = self._conn(srv)
                conn.request("GET", cdn_path)
                resp = conn.getresponse()
                body = resp.read()
                self.assertEqual(resp.status, 200)
                self.assertEqual(body, src_data)
                conn.close()

    def test_range_bytes_first(self):
        with tempfile.TemporaryDirectory() as td:
            with http_server(td, advertise_host="127.0.0.1") as srv:
                provider = srv._server.provider
                cdn_path = provider.cdn_path("myboard")
                conn = self._conn(srv)
                conn.request("GET", cdn_path, headers={"Range": "bytes=0-0"})
                resp = conn.getresponse()
                body = resp.read()
                self.assertEqual(resp.status, 206)
                self.assertEqual(len(body), 1)
                cr = resp.getheader("Content-Range")
                self.assertTrue(cr.startswith("bytes 0-0/"))
                conn.close()

    def test_range_middle(self):
        with tempfile.TemporaryDirectory() as td:
            with http_server(td, advertise_host="127.0.0.1") as srv:
                provider = srv._server.provider
                cdn_path = provider.cdn_path("myboard")
                conn = self._conn(srv)
                conn.request("GET", cdn_path, headers={"Range": "bytes=10-19"})
                resp = conn.getresponse()
                body = resp.read()
                self.assertEqual(resp.status, 206)
                self.assertEqual(len(body), 10)
                cr = resp.getheader("Content-Range")
                self.assertIn("10-19", cr)
                conn.close()

    def test_range_open_ended(self):
        with tempfile.TemporaryDirectory() as td:
            src_data = make_fake_bin("myboard", "v1.0.0")
            pathlib.Path(td, "myboard.bin").write_bytes(src_data)
            with OtaTestServer(td, use_http=True, advertise_host="127.0.0.1").start() as srv:
                provider = srv._server.provider
                cdn_path = provider.cdn_path("myboard")
                n = 50
                conn = self._conn(srv)
                conn.request("GET", cdn_path, headers={"Range": f"bytes={n}-"})
                resp = conn.getresponse()
                body = resp.read()
                self.assertEqual(resp.status, 206)
                self.assertEqual(body, src_data[n:])
                conn.close()

    def test_no_redirect(self):
        with tempfile.TemporaryDirectory() as td:
            src_data = make_fake_bin("myboard", "v1.0.0")
            pathlib.Path(td, "myboard.bin").write_bytes(src_data)
            with OtaTestServer(td, use_http=True, advertise_host="127.0.0.1",
                               no_redirect=True).start() as srv:
                tag = srv.tag
                conn = self._conn(srv)
                conn.request("GET", f"/local/firmware/releases/download/{tag}/myboard.bin")
                resp = conn.getresponse()
                body = resp.read()
                self.assertEqual(resp.status, 200)
                self.assertEqual(body, src_data)
                conn.close()

    def test_context_manager_start_stop(self):
        with tempfile.TemporaryDirectory() as td:
            write_fake_bin(td, "myboard.bin", "myboard", "v1.0.0")
            srv = OtaTestServer(td, use_http=True, advertise_host="127.0.0.1")
            with srv:
                self.assertIsNotNone(srv.releases_url)
                conn = self._conn(srv)
                conn.request("GET", "/releases/latest")
                resp = conn.getresponse()
                resp.read()
                self.assertEqual(resp.status, 200)
                conn.close()
            # After exit the server should be stopped
            self.assertIsNone(srv._server)


# ---------------------------------------------------------------------------
# TLS test
# ---------------------------------------------------------------------------

class TestTLSServer(unittest.TestCase):

    def test_tls_manifest(self):
        with tempfile.TemporaryDirectory() as td:
            write_fake_bin(td, "myboard.bin", "myboard", "v1.0.0")
            with OtaTestServer(td, use_http=False, advertise_host="127.0.0.1").start() as srv:
                cert_path = srv.cert_path
                self.assertIsNotNone(cert_path)
                ctx = ssl.create_default_context(cafile=cert_path)
                conn = http.client.HTTPSConnection("127.0.0.1", srv.port,
                                                   context=ctx, timeout=10)
                conn.request("GET", "/releases/latest")
                resp = conn.getresponse()
                data = json.loads(resp.read())
                self.assertEqual(resp.status, 200)
                self.assertIn("tag_name", data)
                self.assertIsNotNone(srv.fingerprint)
                conn.close()


# ---------------------------------------------------------------------------
# Provider registry tests
# ---------------------------------------------------------------------------

import ota_providers
from ota_providers import ProviderRegistry, build_registry, load_external


class TestProviderRegistry(unittest.TestCase):

    def test_builtin_discovery_finds_github(self):
        registry = build_registry()
        self.assertIn("github", registry.names())

    def test_registry_get_returns_class(self):
        registry = build_registry()
        cls = registry.get("github")
        self.assertIsNotNone(cls)
        # Instantiating should work and produce a Provider
        provider = cls()
        self.assertIsNotNone(provider)

    def test_registry_get_unknown_returns_none(self):
        registry = build_registry()
        self.assertIsNone(registry.get("nonexistent"))

    def test_registry_names_is_sorted(self):
        registry = ProviderRegistry()
        registry.register("zebra", object)
        registry.register("alpha", object)
        self.assertEqual(registry.names(), ["alpha", "zebra"])

    def test_unknown_provider_error_lists_available(self):
        """_build_server raises with registry.names() in the message."""
        registry = build_registry()
        with tempfile.TemporaryDirectory() as td:
            write_fake_bin(td, "myboard.bin", "myboard", "v1.0.0")
            with self.assertRaises(ValueError) as ctx:
                ota_server.OtaTestServer(
                    td,
                    use_http=True,
                    advertise_host="127.0.0.1",
                    provider_name="nonexistent",
                    registry=registry,
                ).start()
            self.assertIn("nonexistent", str(ctx.exception))
            self.assertIn("github", str(ctx.exception))


class TestExternalProviderDropIn(unittest.TestCase):
    """Prove the drop-in external provider contract works end-to-end."""

    def _write_dummy_provider(self, directory: str) -> str:
        path = os.path.join(directory, "dummy_provider.py")
        pathlib.Path(path).write_text(
            "from ota_providers.base import Provider\n"
            "\n"
            "class DummyProvider(Provider):\n"
            "    head_status = 200\n"
            "    def manifest(self, board_map, *, tag, advertise_base):\n"
            "        return {'tag_name': tag, 'assets': []}\n"
            "    def asset_path(self, board, tag):\n"
            "        return f'/dummy/{board}.bin'\n"
            "    def cdn_path(self, board):\n"
            "        return f'/dummy-cdn/{board}.bin'\n"
            "\n"
            "def register(registry):\n"
            "    registry.register('dummy', DummyProvider)\n"
        )
        return path

    def test_load_external_registers_dummy(self):
        registry = ProviderRegistry()
        with tempfile.TemporaryDirectory() as td:
            plugin_path = self._write_dummy_provider(td)
            load_external([plugin_path], td, registry)
            self.assertIn("dummy", registry.names())
            cls = registry.get("dummy")
            self.assertIsNotNone(cls)
            p = cls()
            self.assertEqual(p.head_status, 200)

    def test_build_registry_with_external_config(self):
        with tempfile.TemporaryDirectory() as td:
            plugin_path = self._write_dummy_provider(td)
            plugin_filename = os.path.basename(plugin_path)
            config = {"ota": {"providers": {"paths": [plugin_filename]}}}
            registry = build_registry(config=config, config_dir=td)
            self.assertIn("dummy", registry.names())
            self.assertIn("github", registry.names())

    def test_load_external_bad_path_warns_and_continues(self):
        registry = ProviderRegistry()
        # Non-existent path should not raise; should print warning
        load_external(["/nonexistent/provider.py"], "/", registry)
        # Registry should still be empty (no crash)
        self.assertEqual(registry.names(), [])

    def test_external_provider_serves_via_ota_server(self):
        """External dummy provider can be used with OtaTestServer."""
        with tempfile.TemporaryDirectory() as plugin_dir:
            self._write_dummy_provider(plugin_dir)
            config = {"ota": {"providers": {"paths": ["dummy_provider.py"]}}}
            registry = build_registry(config=config, config_dir=plugin_dir)

            with tempfile.TemporaryDirectory() as td:
                write_fake_bin(td, "myboard.bin", "myboard", "v1.0.0")
                with ota_server.OtaTestServer(
                    td,
                    use_http=True,
                    advertise_host="127.0.0.1",
                    provider_name="dummy",
                    registry=registry,
                ).start() as srv:
                    conn = http.client.HTTPConnection("127.0.0.1", srv.port, timeout=5)
                    conn.request("GET", "/releases/latest")
                    resp = conn.getresponse()
                    data = json.loads(resp.read())
                    self.assertEqual(resp.status, 200)
                    # Dummy provider returns empty assets list
                    self.assertEqual(data["assets"], [])
                    conn.close()


if __name__ == "__main__":
    unittest.main()
