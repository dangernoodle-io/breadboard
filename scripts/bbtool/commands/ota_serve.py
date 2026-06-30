"""ota-serve command — local OTA test server mimicking GitHub+Fastly TLS+redirect+Range flow."""
from __future__ import annotations

import sys

NAME = "ota-serve"
HELP = "Start a local OTA test server mimicking GitHub+Fastly TLS+redirect+Range flow"


def add_arguments(parser) -> None:
    import ota_providers

    registry = ota_providers.build_registry()
    available = registry.names()

    parser.add_argument("--dir", default="dist/",
                        help="directory containing firmware .bin files (default: dist/)")
    parser.add_argument("--board", default=None,
                        help="filter to a single board name")
    parser.add_argument("--host", default="0.0.0.0",
                        help="bind address (default: 0.0.0.0)")
    parser.add_argument("--port", type=int, default=8070,
                        help="listen port (default: 8070)")
    parser.add_argument("--advertise-host", dest="advertise_host", default=None,
                        help="hostname/IP to advertise in URLs (default: auto LAN IP)")
    parser.add_argument("--provider", default="github",
                        help=f"provider topology to emulate (default: github; available: {', '.join(available)})")
    parser.add_argument("--manifest-path", dest="manifest_path",
                        default="/releases/latest",
                        help="manifest URL path (default: /releases/latest)")
    parser.add_argument("--tag", default=None,
                        help="explicit release tag (default: bump patch of highest discovered)")
    parser.add_argument("--no-bump", dest="no_bump", action="store_true",
                        help="serve the real discovered version without bumping patch")
    parser.add_argument("--cert", default=None,
                        help="TLS certificate file (auto-generated if not provided)")
    parser.add_argument("--key", default=None,
                        help="TLS key file (auto-generated if not provided)")
    parser.add_argument("--http", dest="use_http", action="store_true",
                        help="use plain HTTP instead of TLS (for quick local testing)")
    parser.add_argument("--no-redirect", dest="no_redirect", action="store_true",
                        help="serve firmware bytes directly from asset route (no 302)")
    parser.add_argument("--head-ok", dest="head_ok", action="store_true",
                        help="respond 200 to HEAD on CDN route (default: 403 to force Range-GET)")


def register(api) -> None:
    api.add_command(NAME, sys.modules[__name__])


def run(args) -> int:
    import ota_providers
    import ota_server

    registry = ota_providers.build_registry()
    available = registry.names()

    if args.provider not in available:
        print(
            f"Error: unknown provider {args.provider!r}; available: {', '.join(available)}",
            file=sys.stderr,
        )
        return 1

    try:
        srv = ota_server.OtaTestServer(
            args.dir,
            host=args.host,
            port=args.port,
            advertise_host=args.advertise_host,
            provider_name=args.provider,
            manifest_path=args.manifest_path,
            tag=args.tag,
            no_bump=args.no_bump,
            board_filter=args.board,
            cert=args.cert,
            key=args.key,
            use_http=args.use_http,
            no_redirect=args.no_redirect,
            head_ok=args.head_ok,
            registry=registry,
        ).start()
    except ValueError as exc:
        print(f"Error: {exc}", file=sys.stderr)
        return 1

    print()
    print("bbtool ota-serve ready")
    print(f"  releases_url : {srv.releases_url}")
    print(f"  boards       : {', '.join(srv.boards)}")
    print(f"  tag          : {srv.tag}")
    if srv.cert_path:
        print(f"  cert         : {srv.cert_path}")
        print(f"  fingerprint  : {srv.fingerprint}")
        print()
        print("curl examples:")
        print(f"  curl --cacert {srv.cert_path} {srv.releases_url}")
        for board in srv.boards:
            print(
                f"  curl --cacert {srv.cert_path} -L -r 0-1023"
                f" {srv.advertise_base}/local/firmware/releases/download/{srv.tag}/{board}.bin"
            )
    print()
    print(f"Serving on {args.host}:{srv.port} (Ctrl-C to stop)")
    print()

    try:
        srv._server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        srv.stop()

    return 0
