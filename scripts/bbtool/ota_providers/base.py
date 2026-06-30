"""Base class for OTA provider topologies."""
from __future__ import annotations

from typing import Dict, Tuple


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
