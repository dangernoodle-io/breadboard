"""ELF archive store — framework-agnostic; importable without bbtool CLI.

Archive root resolution (precedence):
  1. explicit archive_dir argument
  2. env BBTOOL_ELF_ARCHIVE
  3. caller-supplied config dict: config["archive_dir"]
  4. default ~/.bb/elf-archive/

Archive layout:
  <root>/<sha256>.elf   — ELF binary (SHA256 of ELF bytes)
  <root>/<sha256>.json  — sidecar: board, version, sha256, archived_at, …

Archive key:
  SHA256 of the ELF file bytes — matches what ESP-IDF / esptool embeds in the
  firmware image as esp_app_desc_t.app_elf_sha256 (first N hex chars).

esp_app_desc_t layout (ESP-IDF ≥ 4.x; magic 0xABCD5432, LE uint32 at offset 0):
  offset  0: uint32  magic_word      (0xABCD5432)
  offset  4: uint32  secure_version
  offset  8: uint8[8]  reserv1
  offset 16: char[32]  version
  offset 48: char[32]  project_name
  offset 80: char[16]  time
  offset 96: char[16]  date

Note: app_elf_sha256 in the struct is zeros in the .elf file — esptool fills it
at .bin packaging time.  The archive key equals what /api/info build.app_sha256
reports.
"""
from __future__ import annotations

import hashlib
import json
import logging
import os
import struct
import time
from collections import defaultdict
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Dict, List, Optional, Set, Tuple

logger = logging.getLogger(__name__)

DEFAULT_KEEP = 10

_ESP_APP_DESC_MAGIC = 0xABCD5432
_ESP_APP_DESC_MAGIC_BYTES = struct.pack("<I", _ESP_APP_DESC_MAGIC)

_DESC_OFF_VERSION = 16
_DESC_OFF_PROJECT_NAME = 48
_DESC_OFF_TIME = 80
_DESC_OFF_DATE = 96


@dataclass
class AppDesc:
    """Fields extracted from esp_app_desc_t embedded in the ELF."""
    version: str
    project_name: str
    build_time: str
    build_date: str


@dataclass
class ElfMeta:
    """Sidecar metadata stored alongside each archived ELF."""
    sha256: str
    board: str
    version: str
    build_time: str
    git_sha: str
    dirty: bool
    archived_at: str


def parse_esp_app_desc(elf_data: bytes) -> Optional[AppDesc]:
    """Extract esp_app_desc_t from raw ELF bytes; returns None when magic absent."""
    idx = elf_data.find(_ESP_APP_DESC_MAGIC_BYTES)
    if idx < 0:
        return None
    chunk = elf_data[idx: idx + 256]
    if len(chunk) < 112:
        return None

    def _cstr(data: bytes, off: int, length: int) -> str:
        return data[off: off + length].rstrip(b"\x00").decode("utf-8", errors="replace")

    return AppDesc(
        version=_cstr(chunk, _DESC_OFF_VERSION, 32),
        project_name=_cstr(chunk, _DESC_OFF_PROJECT_NAME, 32),
        build_time=_cstr(chunk, _DESC_OFF_TIME, 16),
        build_date=_cstr(chunk, _DESC_OFF_DATE, 16),
    )


def _resolve_archive_root(
    archive_dir: Optional[str] = None,
    config: Optional[dict] = None,
) -> Path:
    """Resolve and create the archive directory (precedence: arg > env > config > default)."""
    if archive_dir:
        root = Path(archive_dir).expanduser()
    elif "BBTOOL_ELF_ARCHIVE" in os.environ:
        root = Path(os.environ["BBTOOL_ELF_ARCHIVE"]).expanduser()
    elif config and config.get("archive_dir"):
        root = Path(config["archive_dir"]).expanduser()
    else:
        root = Path.home() / ".bb" / "elf-archive"
    root.mkdir(parents=True, exist_ok=True)
    return root


def sha256_of_elf(elf_path: str) -> str:
    """SHA256 of elf_path bytes — same value esp_app_desc_t.app_elf_sha256 holds."""
    h = hashlib.sha256()
    with open(elf_path, "rb") as fh:
        for chunk in iter(lambda: fh.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def archive(
    elf_path: str,
    board: str = "",
    version: str = "",
    build_time: str = "",
    git_sha: str = "",
    dirty: bool = False,
    archive_dir: Optional[str] = None,
    config: Optional[dict] = None,
    keep: int = DEFAULT_KEEP,
    auto_prune: bool = True,
    _archived_at: Optional[str] = None,
) -> str:
    """Copy elf_path into the archive and return its full SHA256 key.

    Idempotent: same SHA updates the sidecar only.
    board/version auto-populated from esp_app_desc_t when not supplied.
    auto_prune=True (default) applies keep-last-N-per-board after writing;
    pass keep=0 or auto_prune=False to opt out.
    """
    root = _resolve_archive_root(archive_dir, config)
    key = sha256_of_elf(elf_path)
    dest_elf = root / f"{key}.elf"
    dest_json = root / f"{key}.json"

    elf_bytes: Optional[bytes] = None
    if not dest_elf.exists():
        import shutil
        shutil.copy2(elf_path, dest_elf)
        logger.debug("elfstore: archived %s -> %s", elf_path, dest_elf)
    else:
        logger.debug("elfstore: %s already archived", key[:16])

    if not board or not version:
        try:
            if elf_bytes is None:
                with open(elf_path, "rb") as fh:
                    elf_bytes = fh.read()
            desc = parse_esp_app_desc(elf_bytes)
            if desc is not None:
                if not board:
                    board = desc.project_name
                if not version:
                    version = desc.version
                if not build_time:
                    build_time = f"{desc.build_date} {desc.build_time}".strip()
                logger.debug(
                    "elfstore: auto-populated board=%r version=%r", board, version,
                )
        except Exception as exc:
            logger.debug("elfstore: esp_app_desc parse failed: %s", exc)

    meta = ElfMeta(
        sha256=key,
        board=board,
        version=version,
        build_time=build_time,
        git_sha=git_sha,
        dirty=dirty,
        archived_at=_archived_at if _archived_at is not None else _utc_now(),
    )
    dest_json.write_text(json.dumps(asdict(meta), indent=2))

    if auto_prune and keep > 0:
        prune(keep=keep, archive_dir=archive_dir, config=config)

    return key


def list_entries(
    archive_dir: Optional[str] = None,
    config: Optional[dict] = None,
) -> List[Tuple[ElfMeta, int]]:
    """Return all archived entries sorted by archived_at ascending (oldest first)."""
    root = _resolve_archive_root(archive_dir, config)
    entries: List[Tuple[ElfMeta, int]] = []
    for jpath in root.glob("*.json"):
        elf_p = jpath.with_suffix(".elf")
        if not elf_p.exists():
            continue
        try:
            meta = _load_meta(jpath)
        except Exception as exc:
            logger.warning("elfstore: bad sidecar %s: %s", jpath, exc)
            continue
        entries.append((meta, elf_p.stat().st_size))
    entries.sort(key=lambda t: t[0].archived_at)
    return entries


def find(
    sha_prefix: str,
    archive_dir: Optional[str] = None,
    config: Optional[dict] = None,
) -> Optional[str]:
    """Find archived ELF path by SHA prefix (e.g. 9-char from /api/diag/panic).

    Returns absolute path string, or None if not found.
    On multiple matches, returns the most-recently archived entry.
    """
    sha_prefix = sha_prefix.lower().strip()
    root = _resolve_archive_root(archive_dir, config)
    matches: List[Tuple[str, str]] = []
    for jpath in root.glob("*.json"):
        key = jpath.stem
        if key.startswith(sha_prefix):
            elf_p = jpath.with_suffix(".elf")
            if elf_p.exists():
                try:
                    meta = _load_meta(jpath)
                    matches.append((meta.archived_at, str(elf_p)))
                except Exception:
                    matches.append(("", str(elf_p)))
    if not matches:
        return None
    matches.sort(key=lambda t: t[0], reverse=True)
    if len(matches) > 1:
        logger.warning(
            "elfstore: %d entries match prefix %r; using most-recent",
            len(matches), sha_prefix,
        )
    return matches[0][1]


def prune(
    keep: int = DEFAULT_KEEP,
    max_age: Optional[float] = None,
    in_use_shas: Optional[Set[str]] = None,
    grace_keep: int = 0,
    dry_run: bool = False,
    archive_dir: Optional[str] = None,
    config: Optional[dict] = None,
) -> List[str]:
    """Remove old ELF entries exceeding budget.

    keep        — keep the N most-recently archived entries per board (0 = skip count pruning).
    max_age     — also remove entries older than max_age seconds.
    in_use_shas — full sha256 keys to never delete (deployed-firmware protection).
    grace_keep  — always keep the N most-recently archived entries globally.
    dry_run     — return what would be deleted without deleting.

    Returns list of deleted sha256 keys.
    """
    entries = list_entries(archive_dir=archive_dir, config=config)  # oldest-first
    now = time.time()

    candidates_by_count: Set[str] = set()
    if keep > 0:
        board_map: Dict[str, List[ElfMeta]] = defaultdict(list)
        for meta, _ in entries:
            board_map[meta.board].append(meta)
        for board_metas in board_map.values():
            by_recency = sorted(board_metas, key=lambda m: m.archived_at, reverse=True)
            for m in by_recency[keep:]:
                candidates_by_count.add(m.sha256)

    candidates_by_age: Set[str] = set()
    if max_age is not None:
        cutoff = now - max_age
        for meta, _ in entries:
            try:
                if _parse_ts(meta.archived_at) < cutoff:
                    candidates_by_age.add(meta.sha256)
            except Exception:
                continue

    to_delete = candidates_by_count | candidates_by_age

    if in_use_shas:
        to_delete -= set(in_use_shas)

    if grace_keep > 0:
        recent = sorted(entries, key=lambda t: t[0].archived_at, reverse=True)
        for meta, _ in recent[:grace_keep]:
            to_delete.discard(meta.sha256)

    root = _resolve_archive_root(archive_dir, config)
    deleted: List[str] = []
    for meta, _ in entries:
        if meta.sha256 not in to_delete:
            continue
        elf_p = root / f"{meta.sha256}.elf"
        json_p = root / f"{meta.sha256}.json"
        if dry_run:
            logger.info("elfstore: [dry-run] would delete %s", meta.sha256[:16])
        else:
            for p in (elf_p, json_p):
                try:
                    p.unlink(missing_ok=True)
                except Exception as exc:
                    logger.warning("elfstore: delete %s: %s", p, exc)
            logger.debug("elfstore: pruned %s (board=%s)", meta.sha256[:16], meta.board)
        deleted.append(meta.sha256)

    return deleted


def _load_meta(json_path: Path) -> ElfMeta:
    d = json.loads(json_path.read_text())
    return ElfMeta(
        sha256=d.get("sha256", json_path.stem),
        board=d.get("board", ""),
        version=d.get("version", ""),
        build_time=d.get("build_time", ""),
        git_sha=d.get("git_sha", ""),
        dirty=bool(d.get("dirty", False)),
        archived_at=d.get("archived_at", ""),
    )


def _utc_now() -> str:
    import datetime
    return datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def _parse_ts(s: str) -> float:
    import datetime
    s = s.rstrip("Z")
    dt = datetime.datetime.fromisoformat(s)
    return dt.replace(tzinfo=datetime.timezone.utc).timestamp()
