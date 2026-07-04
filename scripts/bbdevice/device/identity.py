"""Device identity read via /api/info (HTTP, not mDNS).

Extracted from fleet's fleetlib/discovery.py so that safety.py can depend on
identity without depending on the mDNS-coupled discovery module.
"""
from __future__ import annotations
from typing import Optional, Tuple, TYPE_CHECKING

from .client import Client, TIMEOUT_INFO, info_field

if TYPE_CHECKING:
    from .discovery import Device


def _read_identity(device: "Device") -> Tuple[Optional[str], Optional[str]]:
    """Fetch /api/info and return (board, hostname).

    Returns (None, None) when the device is unreachable (info=None).
    Returns (board, hostname) — either or both may be a non-None string — when
    the device responds.  Used by safety.Guard to distinguish unreachable from
    a genuine identity mismatch.
    """
    c = Client(device.ip, device.port)
    info = c.get_json("/api/info", timeout=TIMEOUT_INFO)
    if info is None:
        return None, None
    board = info_field(info, "board")
    hostname = info.get("hostname") or info.get("host")
    return board, hostname
