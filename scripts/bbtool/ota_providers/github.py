"""GitHub Releases + Fastly CDN provider for OTA test server."""
from __future__ import annotations

import secrets
from typing import Dict, Tuple

from .base import Provider


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


def register(registry) -> None:
    registry.register("github", GitHubProvider)
