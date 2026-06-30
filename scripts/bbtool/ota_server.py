"""OTA test server — framework-agnostic; importable without bbtool CLI.

Discovers firmware .bin files by parsing esp_app_desc_t magic (0xABCD5432),
builds a board→(path,version) map, and serves a TLS+redirect+Range flow that
mimics the GitHub Releases / Fastly CDN topology used by esp_https_ota.

Topology (GitHub provider):
  GET /releases/latest                          → JSON manifest
  GET /local/firmware/releases/download/<t>/b  → 302 to CDN route
  HEAD /cdn/<token>/b.bin                       → 403 (forces Range-GET fallback)
  GET  /cdn/<token>/b.bin [Range: bytes=N-M]   → 206 with Content-Range

TLS: auto-generates a self-signed cert+key via openssl subprocess with SAN for
     advertise-host; cached in ~/.bb/ota-serve/<host>/; SHA-256 fingerprint
     printed on startup.  Pass --cert/--key for BYO certs; --http for plain mode.

Embeddable API:
    with OtaTestServer("dist/") as srv:
        # srv.releases_url  srv.port  srv.cert_path  srv.fingerprint
        ...
"""
from __future__ import annotations

import http.server
import json
import logging
import os
import pathlib
import re
import secrets
import socket
import socketserver
import ssl
import struct
import subprocess
import threading
from typing import Dict, List, Optional, Tuple

logger = logging.getLogger(__name__)

# ---------------------------------------------------------------------------
# esp_app_desc_t parsing
# ---------------------------------------------------------------------------

_APP_DESC_MAGIC = 0xABCD5432
_APP_DESC_MAGIC_BYTES = struct.pack("<I", _APP_DESC_MAGIC)
# Layout from magic offset: magic(4) + secure_version(4) + reserv1(8) + version(32) + project_name(32)
_APP_DESC_STRUCT = struct.Struct("<I I 8s 32s 32s")


def parse_app_desc_from_bin(path: str) -> Tuple[str, str]:
    """Parse esp_app_desc_t from a .bin file.

    Returns (project_name, version).
    Raises ValueError if magic not found or struct too short.
    """
    with open(path, "rb") as fh:
        data = fh.read()

    idx = data.find(_APP_DESC_MAGIC_BYTES)
    if idx < 0:
        raise ValueError(f"esp_app_desc_t magic not found in {path}")

    chunk = data[idx: idx + _APP_DESC_STRUCT.size]
    if len(chunk) < _APP_DESC_STRUCT.size:
        raise ValueError(f"esp_app_desc_t struct truncated in {path}")

    _magic, _secure_ver, _reserv1, ver_bytes, name_bytes = _APP_DESC_STRUCT.unpack(chunk)
    version = ver_bytes.rstrip(b"\x00").decode("utf-8", errors="replace")
    project_name = name_bytes.rstrip(b"\x00").decode("utf-8", errors="replace")
    return project_name, version


# ---------------------------------------------------------------------------
# Semver helpers
# ---------------------------------------------------------------------------

def _parse_semver(v: str) -> Optional[Tuple[int, int, int]]:
    """Parse 'vX.Y.Z' or 'X.Y.Z' → (X, Y, Z).

    Dev builds (starting with 'dev' after stripping 'v') return (0, 0, 0).
    Returns None if not parseable.
    """
    s = v.lstrip("v")
    if s.lower().startswith("dev"):
        return (0, 0, 0)
    m = re.match(r"^(\d+)\.(\d+)\.(\d+)", s)
    if not m:
        return None
    return (int(m.group(1)), int(m.group(2)), int(m.group(3)))


def compute_tag(versions: List[str], *, tag: Optional[str] = None, no_bump: bool = False) -> str:
    """Compute the release tag from discovered firmware versions.

    - tag:     explicit override → returned as-is
    - no_bump: return highest parsed semver (or 'v0.0.0' if none)
    - default: bump patch of highest; 'v99.99.99' sentinel if no valid semver
    """
    if tag:
        return tag

    parsed: List[Tuple[Tuple[int, int, int], str]] = []
    for v in versions:
        t = _parse_semver(v)
        if t is not None:
            parsed.append((t, v))

    if not parsed:
        return "v0.0.0" if no_bump else "v99.99.99"

    parsed.sort(key=lambda x: x[0], reverse=True)
    best_tuple, best_str = parsed[0]

    if no_bump:
        # Normalise to vX.Y.Z
        ma, mi, pa = best_tuple
        return f"v{ma}.{mi}.{pa}"

    ma, mi, pa = best_tuple
    return f"v{ma}.{mi}.{pa + 1}"


# ---------------------------------------------------------------------------
# Discovery
# ---------------------------------------------------------------------------

def discover_bins(serve_dir: str) -> Dict[str, Tuple[str, str]]:
    """Glob *.bin in serve_dir; parse each for esp_app_desc_t.

    Returns {project_name: (abs_path, version)}.
    Silently skips files whose magic cannot be found.
    """
    result: Dict[str, Tuple[str, str]] = {}
    p = pathlib.Path(serve_dir)
    for bin_path in sorted(p.glob("*.bin")):
        try:
            project_name, version = parse_app_desc_from_bin(str(bin_path))
            if project_name:
                result[project_name] = (str(bin_path.resolve()), version)
        except Exception as exc:
            logger.debug("discover_bins: skipping %s: %s", bin_path.name, exc)
    return result


# ---------------------------------------------------------------------------
# Provider abstraction
# ---------------------------------------------------------------------------

class Provider:
    """Base class for OTA provider topologies."""
    head_status: int = 403

    def manifest(
        self,
        board_map: Dict[str, Tuple[str, str]],
        *,
        tag: str,
        advertise_base: str,
    ) -> dict:
        raise NotImplementedError

    def asset_path(self, board: str, tag: str) -> str:
        raise NotImplementedError

    def cdn_path(self, board: str) -> str:
        raise NotImplementedError


class GitHubProvider(Provider):
    """Mimics GitHub Releases + Fastly CDN topology.

    - manifest:   {"tag_name": tag, "assets": [{"name": "<board>.bin",
                   "browser_download_url": "<advertise_base><asset_path>"}]}
    - asset_path: /local/firmware/releases/download/<tag>/<board>.bin
    - cdn_path:   /cdn/<random_token>/<board>.bin  (stable per server instance)
    """
    head_status = 403

    def __init__(self) -> None:
        self._token = secrets.token_hex(16)

    def manifest(
        self,
        board_map: Dict[str, Tuple[str, str]],
        *,
        tag: str,
        advertise_base: str,
    ) -> dict:
        assets = [
            {
                "name": f"{board}.bin",
                "browser_download_url": f"{advertise_base}{self.asset_path(board, tag)}",
            }
            for board in sorted(board_map.keys())
        ]
        return {"tag_name": tag, "assets": assets}

    def asset_path(self, board: str, tag: str) -> str:
        return f"/local/firmware/releases/download/{tag}/{board}.bin"

    def cdn_path(self, board: str) -> str:
        return f"/cdn/{self._token}/{board}.bin"


PROVIDERS: Dict[str, type] = {"github": GitHubProvider}


# ---------------------------------------------------------------------------
# TLS helpers
# ---------------------------------------------------------------------------

def _get_fingerprint(cert_path: str) -> str:
    result = subprocess.run(
        ["openssl", "x509", "-fingerprint", "-sha256", "-noout", "-in", cert_path],
        capture_output=True, text=True, check=True,
    )
    return result.stdout.strip()


def make_ssl_context(
    advertise_host: str,
    cert: Optional[str] = None,
    key: Optional[str] = None,
) -> Tuple[ssl.SSLContext, str, str]:
    """Return (ssl_ctx, cert_path, fingerprint).

    If cert+key provided, use them directly (fingerprint computed from cert).
    Otherwise auto-generate a self-signed cert with SAN for advertise_host,
    cached in ~/.bb/ota-serve/<advertise_host>/.
    """
    if cert and key:
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(cert, key)
        fingerprint = _get_fingerprint(cert)
        return ctx, cert, fingerprint

    cache_dir = pathlib.Path.home() / ".bb" / "ota-serve" / advertise_host
    cache_dir.mkdir(parents=True, exist_ok=True)
    cert_path = cache_dir / "cert.pem"
    key_path = cache_dir / "key.pem"

    if not cert_path.exists() or not key_path.exists():
        try:
            socket.inet_aton(advertise_host)
            san = f"IP:{advertise_host}"
        except OSError:
            san = f"DNS:{advertise_host}"

        subprocess.run([
            "openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
            "-days", "365",
            "-keyout", str(key_path),
            "-out", str(cert_path),
            "-subj", f"/CN={advertise_host}",
            "-addext", f"subjectAltName={san}",
        ], check=True, capture_output=True)
        logger.info("generated self-signed cert: %s", cert_path)

    fingerprint = _get_fingerprint(str(cert_path))
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(str(cert_path), str(key_path))
    return ctx, str(cert_path), fingerprint


# ---------------------------------------------------------------------------
# LAN IP
# ---------------------------------------------------------------------------

def get_lan_ip() -> str:
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))
        return s.getsockname()[0]
    finally:
        s.close()


# ---------------------------------------------------------------------------
# HTTP Handler
# ---------------------------------------------------------------------------

class OTAHandler(http.server.BaseHTTPRequestHandler):
    """HTTP handler for OTA test server.

    server attributes consumed:
      provider, board_map, tag, manifest_path_prefix, no_redirect,
      head_ok, advertise_base, use_tls, manifest_data,
      asset_pattern, cdn_pattern
    """

    def do_GET(self) -> None:
        self._dispatch("GET")

    def do_HEAD(self) -> None:
        self._dispatch("HEAD")

    def _dispatch(self, method: str) -> None:
        path = self.path.split("?")[0]
        if path == self.server.manifest_path_prefix:
            self._serve_manifest(method)
        elif self.server.asset_pattern.match(path):
            self._serve_asset(method, path)
        elif self.server.cdn_pattern.match(path):
            self._serve_cdn(method, path)
        else:
            self.send_error(404)

    def _serve_manifest(self, method: str) -> None:
        data = json.dumps(self.server.manifest_data).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        if method == "GET":
            self.wfile.write(data)

    def _board_from_asset_path(self, path: str) -> Optional[str]:
        m = self.server.asset_pattern.match(path)
        return m.group(1) if m else None

    def _board_from_cdn_path(self, path: str) -> Optional[str]:
        m = self.server.cdn_pattern.match(path)
        return m.group(1) if m else None

    def _serve_asset(self, method: str, path: str) -> None:
        board = self._board_from_asset_path(path)
        if board is None or board not in self.server.board_map:
            self.send_error(404)
            return
        if self.server.no_redirect:
            bin_path, _ = self.server.board_map[board]
            self._serve_bytes(bin_path, method)
        else:
            cdn_url = self.server.advertise_base + self.server.provider.cdn_path(board)
            self.send_response(302)
            self.send_header("Location", cdn_url)
            self.send_header("Content-Length", "0")
            self.end_headers()
            logger.info("302 %s -> %s", path, cdn_url)

    def _serve_cdn(self, method: str, path: str) -> None:
        board = self._board_from_cdn_path(path)
        if board is None or board not in self.server.board_map:
            self.send_error(404)
            return
        bin_path, _ = self.server.board_map[board]
        if method == "HEAD" and not self.server.head_ok:
            size = os.path.getsize(bin_path)
            self.send_response(self.server.provider.head_status)
            self.send_header("Content-Length", str(size))
            self.end_headers()
            logger.info("HEAD %s -> %d", path, self.server.provider.head_status)
        else:
            self._serve_bytes(bin_path, method)

    def _serve_bytes(self, path: str, method: str) -> None:
        with open(path, "rb") as fh:
            data = fh.read()
        total = len(data)
        range_hdr = self.headers.get("Range", "")
        if range_hdr.startswith("bytes="):
            spec = range_hdr[6:]
            if "-" in spec:
                s, e = spec.split("-", 1)
                start = int(s) if s else 0
                end = int(e) if e else total - 1
                end = min(end, total - 1)
                chunk = data[start: end + 1]
                self.send_response(206)
                self.send_header("Content-Range", f"bytes {start}-{end}/{total}")
                self.send_header("Content-Length", str(len(chunk)))
                self.send_header("Accept-Ranges", "bytes")
                self.end_headers()
                if method == "GET":
                    self.wfile.write(chunk)
                logger.info("GET range %d-%d/%d -> 206", start, end, total)
                return
        self.send_response(200)
        self.send_header("Content-Length", str(total))
        self.send_header("Accept-Ranges", "bytes")
        self.end_headers()
        if method == "GET":
            self.wfile.write(data)
        logger.info("GET full %d bytes -> 200", total)

    def log_message(self, fmt: str, *args) -> None:
        logging.info("%s - %s", self.address_string(), fmt % args)


# ---------------------------------------------------------------------------
# Raw HTTP server
# ---------------------------------------------------------------------------

class _OTARawServer(socketserver.ThreadingMixIn, http.server.HTTPServer):
    """Threaded HTTP server for OTA serving."""
    daemon_threads = True


# ---------------------------------------------------------------------------
# Embeddable OtaTestServer
# ---------------------------------------------------------------------------

class OtaTestServer:
    """Embeddable OTA test server with context-manager support.

    Usage:
        with OtaTestServer("dist/", use_http=True) as srv:
            print(srv.releases_url)
            print(srv.port)

        # Or manually:
        srv = OtaTestServer("dist/")
        srv.start()   # non-blocking
        ...
        srv.stop()

    Constructor args mirror run_server() kwargs.
    """

    def __init__(
        self,
        serve_dir: str,
        *,
        host: str = "0.0.0.0",
        port: int = 0,
        advertise_host: Optional[str] = None,
        provider_name: str = "github",
        manifest_path: str = "/releases/latest",
        tag: Optional[str] = None,
        no_bump: bool = False,
        board_filter: Optional[str] = None,
        cert: Optional[str] = None,
        key: Optional[str] = None,
        use_http: bool = False,
        no_redirect: bool = False,
        head_ok: bool = False,
    ) -> None:
        self._serve_dir = serve_dir
        self._host = host
        self._port = port
        self._advertise_host = advertise_host
        self._provider_name = provider_name
        self._manifest_path = manifest_path
        self._tag = tag
        self._no_bump = no_bump
        self._board_filter = board_filter
        self._cert = cert
        self._key = key
        self._use_http = use_http
        self._no_redirect = no_redirect
        self._head_ok = head_ok

        self._server: Optional[_OTARawServer] = None
        self._thread: Optional[threading.Thread] = None
        self._info: Optional[dict] = None

    # -- public properties (available after start()) --------------------------

    @property
    def releases_url(self) -> str:
        self._assert_started()
        return self._info["releases_url"]  # type: ignore[index]

    @property
    def port(self) -> int:
        self._assert_started()
        return self._info["port"]  # type: ignore[index]

    @property
    def cert_path(self) -> Optional[str]:
        self._assert_started()
        return self._info["cert_path"]  # type: ignore[index]

    @property
    def fingerprint(self) -> Optional[str]:
        self._assert_started()
        return self._info["fingerprint"]  # type: ignore[index]

    @property
    def advertise_base(self) -> str:
        self._assert_started()
        return self._info["advertise_base"]  # type: ignore[index]

    @property
    def boards(self) -> List[str]:
        self._assert_started()
        return self._info["boards"]  # type: ignore[index]

    @property
    def tag(self) -> str:
        self._assert_started()
        return self._info["tag"]  # type: ignore[index]

    # -- lifecycle ------------------------------------------------------------

    def start(self) -> "OtaTestServer":
        """Bind and start serving in a background daemon thread (non-blocking)."""
        if self._server is not None:
            return self
        self._info = _build_server(
            serve_dir=self._serve_dir,
            host=self._host,
            port=self._port,
            advertise_host=self._advertise_host,
            provider_name=self._provider_name,
            manifest_path=self._manifest_path,
            tag=self._tag,
            no_bump=self._no_bump,
            board_filter=self._board_filter,
            cert=self._cert,
            key=self._key,
            use_http=self._use_http,
            no_redirect=self._no_redirect,
            head_ok=self._head_ok,
        )
        self._server = self._info["server"]
        self._thread = threading.Thread(
            target=self._server.serve_forever, daemon=True, name="ota-test-server"
        )
        self._thread.start()
        return self

    def stop(self) -> None:
        """Shutdown the server and join the serving thread."""
        if self._server is not None:
            self._server.shutdown()
            self._server.server_close()
            if self._thread is not None:
                self._thread.join(timeout=5)
            self._server = None
            self._thread = None

    def close(self) -> None:
        """Alias for stop()."""
        self.stop()

    def __enter__(self) -> "OtaTestServer":
        return self.start()

    def __exit__(self, *_) -> None:
        self.stop()

    def _assert_started(self) -> None:
        if self._info is None:
            raise RuntimeError("OtaTestServer not started; call start() or use as context manager")


# ---------------------------------------------------------------------------
# Internal server builder (shared by OtaTestServer and legacy run_server)
# ---------------------------------------------------------------------------

def _build_server(
    serve_dir: str,
    *,
    host: str,
    port: int,
    advertise_host: Optional[str],
    provider_name: str,
    manifest_path: str,
    tag: Optional[str],
    no_bump: bool,
    board_filter: Optional[str],
    cert: Optional[str],
    key: Optional[str],
    use_http: bool,
    no_redirect: bool,
    head_ok: bool,
) -> dict:
    """Build and return the server info dict (does NOT start serving)."""
    board_map = discover_bins(serve_dir)
    if not board_map:
        raise ValueError(f"No valid firmware .bin files found in {serve_dir}")

    if board_filter:
        board_map = {k: v for k, v in board_map.items() if k == board_filter}
        if not board_map:
            raise ValueError(f"Board {board_filter!r} not found in {serve_dir}")

    provider_cls = PROVIDERS.get(provider_name)
    if provider_cls is None:
        raise ValueError(f"Unknown provider {provider_name!r}; choices: {list(PROVIDERS)}")
    provider = provider_cls()

    computed_tag = compute_tag(
        [v for _, v in board_map.values()],
        tag=tag,
        no_bump=no_bump,
    )

    if advertise_host is None:
        advertise_host = get_lan_ip()

    scheme = "http" if use_http else "https"
    # Initial advertise_base; may be updated after bind if port=0
    advertise_base = f"{scheme}://{advertise_host}:{port}"

    server = _OTARawServer((host, port), OTAHandler)

    # Get the OS-assigned port (works for both port=0 and explicit ports)
    actual_port = server.server_address[1]
    advertise_base = f"{scheme}://{advertise_host}:{actual_port}"

    manifest_data = provider.manifest(board_map, tag=computed_tag, advertise_base=advertise_base)

    server.provider = provider  # type: ignore[attr-defined]
    server.board_map = board_map  # type: ignore[attr-defined]
    server.tag = computed_tag  # type: ignore[attr-defined]
    server.manifest_path_prefix = manifest_path  # type: ignore[attr-defined]
    server.no_redirect = no_redirect  # type: ignore[attr-defined]
    server.head_ok = head_ok  # type: ignore[attr-defined]
    server.advertise_base = advertise_base  # type: ignore[attr-defined]
    server.use_tls = not use_http  # type: ignore[attr-defined]
    server.manifest_data = manifest_data  # type: ignore[attr-defined]
    server.asset_pattern = re.compile(r".*/releases/download/[^/]+/(.+)\.bin$")  # type: ignore[attr-defined]
    server.cdn_pattern = re.compile(r"/cdn/[^/]+/(.+)\.bin$")  # type: ignore[attr-defined]

    cert_path: Optional[str] = None
    fingerprint: Optional[str] = None

    if not use_http:
        ssl_ctx, cert_path, fingerprint = make_ssl_context(advertise_host, cert, key)
        server.socket = ssl_ctx.wrap_socket(server.socket, server_side=True)

    releases_url = f"{advertise_base}{manifest_path}"

    return {
        "server": server,
        "releases_url": releases_url,
        "cert_path": cert_path,
        "fingerprint": fingerprint,
        "advertise_base": advertise_base,
        "boards": list(board_map.keys()),
        "tag": computed_tag,
        "port": actual_port,
    }


# ---------------------------------------------------------------------------
# Legacy functional API (kept for compatibility)
# ---------------------------------------------------------------------------

def run_server(
    serve_dir: str,
    *,
    host: str = "0.0.0.0",
    port: int = 8070,
    advertise_host: Optional[str] = None,
    provider_name: str = "github",
    manifest_path: str = "/releases/latest",
    tag: Optional[str] = None,
    no_bump: bool = False,
    board_filter: Optional[str] = None,
    cert: Optional[str] = None,
    key: Optional[str] = None,
    use_http: bool = False,
    no_redirect: bool = False,
    head_ok: bool = False,
) -> dict:
    """Discover bins, build server, optionally wrap TLS.

    Returns startup info dict (server, releases_url, cert_path, fingerprint,
    advertise_base, boards, tag, port).  Does NOT start serving.
    Call start_server(info) to block, or info['server'].serve_forever() in a thread.
    """
    return _build_server(
        serve_dir=serve_dir,
        host=host,
        port=port,
        advertise_host=advertise_host,
        provider_name=provider_name,
        manifest_path=manifest_path,
        tag=tag,
        no_bump=no_bump,
        board_filter=board_filter,
        cert=cert,
        key=key,
        use_http=use_http,
        no_redirect=no_redirect,
        head_ok=head_ok,
    )


def start_server(info: dict) -> None:
    """Block serving forever (call from command layer)."""
    info["server"].serve_forever()
