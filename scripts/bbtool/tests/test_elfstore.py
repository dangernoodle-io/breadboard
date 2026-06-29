"""Unit tests for elfstore.py — framework-agnostic, no bbtool CLI imports."""
import hashlib
import os
import struct
import sys
import tempfile
import unittest
from pathlib import Path

# Import elfstore directly — proves framework-agnostic (no CLI deps).
sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
import elfstore
from elfstore import (
    AppDesc,
    ElfMeta,
    archive,
    find,
    list_entries,
    parse_esp_app_desc,
    prune,
    _resolve_archive_root,
)


# ---------------------------------------------------------------------------
# helpers
# ---------------------------------------------------------------------------

def _write_elf(directory: str, name: str, content: bytes) -> str:
    path = os.path.join(directory, name)
    with open(path, "wb") as fh:
        fh.write(content)
    return path


def _make_esp_app_desc(
    version: str = "v1.2.3",
    project_name: str = "myproject",
    build_time: str = "12:00:00",
    build_date: str = "Jan 01 2026",
) -> bytes:
    """Craft a minimal binary containing an esp_app_desc_t struct at offset 0."""
    data = bytearray(256)
    data[0:4] = struct.pack("<I", 0xABCD5432)
    # secure_version at 4, reserv1 at 8 — stay zero
    _write_cstr(data, 16, version, 32)
    _write_cstr(data, 48, project_name, 32)
    _write_cstr(data, 80, build_time, 16)
    _write_cstr(data, 96, build_date, 16)
    return bytes(data)


def _write_cstr(buf: bytearray, offset: int, s: str, max_len: int) -> None:
    b = s.encode("utf-8")[:max_len]
    buf[offset: offset + len(b)] = b


# ---------------------------------------------------------------------------
# parse_esp_app_desc
# ---------------------------------------------------------------------------

class TestParseEspAppDesc(unittest.TestCase):

    def test_extracts_version_and_project(self):
        data = _make_esp_app_desc(version="v2.0.0", project_name="smoke-esp32-wroom")
        desc = parse_esp_app_desc(data)
        self.assertIsNotNone(desc)
        self.assertEqual(desc.version, "v2.0.0")
        self.assertEqual(desc.project_name, "smoke-esp32-wroom")

    def test_extracts_build_time_and_date(self):
        data = _make_esp_app_desc(build_time="15:30:00", build_date="Jun 28 2026")
        desc = parse_esp_app_desc(data)
        self.assertIsNotNone(desc)
        self.assertEqual(desc.build_time, "15:30:00")
        self.assertEqual(desc.build_date, "Jun 28 2026")

    def test_magic_not_found_returns_none(self):
        self.assertIsNone(parse_esp_app_desc(b"\x00" * 256))

    def test_magic_embedded_in_larger_blob(self):
        # magic at offset 64 — find() should still locate it
        data = bytearray(b"\xAA" * 64) + bytearray(_make_esp_app_desc(version="v3.0.0"))
        desc = parse_esp_app_desc(bytes(data))
        self.assertIsNotNone(desc)
        self.assertEqual(desc.version, "v3.0.0")

    def test_project_name_stored_as_is(self):
        # project_name must NOT be stripped — stored verbatim
        data = _make_esp_app_desc(project_name="taipanminer-esp32-wroom32")
        desc = parse_esp_app_desc(data)
        self.assertEqual(desc.project_name, "taipanminer-esp32-wroom32")

    def test_truncated_chunk_returns_none(self):
        # Only 10 bytes after the magic — too short
        data = struct.pack("<I", 0xABCD5432) + b"\x00" * 10
        self.assertIsNone(parse_esp_app_desc(data))


# ---------------------------------------------------------------------------
# archive root resolution precedence
# ---------------------------------------------------------------------------

class TestArchiveDirResolution(unittest.TestCase):

    def setUp(self):
        self._orig_env = os.environ.pop("BBTOOL_ELF_ARCHIVE", None)

    def tearDown(self):
        os.environ.pop("BBTOOL_ELF_ARCHIVE", None)
        if self._orig_env is not None:
            os.environ["BBTOOL_ELF_ARCHIVE"] = self._orig_env

    def test_arg_wins_over_env(self):
        with tempfile.TemporaryDirectory() as td_arg:
            with tempfile.TemporaryDirectory() as td_env:
                os.environ["BBTOOL_ELF_ARCHIVE"] = td_env
                root = _resolve_archive_root(archive_dir=td_arg)
                self.assertEqual(root, Path(td_arg))

    def test_env_wins_over_config(self):
        with tempfile.TemporaryDirectory() as td_env:
            with tempfile.TemporaryDirectory() as td_cfg:
                os.environ["BBTOOL_ELF_ARCHIVE"] = td_env
                root = _resolve_archive_root(config={"archive_dir": td_cfg})
                self.assertEqual(root, Path(td_env))

    def test_config_wins_over_default(self):
        with tempfile.TemporaryDirectory() as td_cfg:
            root = _resolve_archive_root(config={"archive_dir": td_cfg})
            self.assertEqual(root, Path(td_cfg))

    def test_creates_directory_if_missing(self):
        with tempfile.TemporaryDirectory() as td:
            new_dir = os.path.join(td, "nested", "archive")
            root = _resolve_archive_root(archive_dir=new_dir)
            self.assertTrue(root.exists())


# ---------------------------------------------------------------------------
# archive → find round-trip
# ---------------------------------------------------------------------------

class TestArchiveFindRoundTrip(unittest.TestCase):

    def test_find_by_prefix(self):
        with tempfile.TemporaryDirectory() as td_elfs:
            with tempfile.TemporaryDirectory() as td_store:
                elf = _write_elf(td_elfs, "firmware.elf", b"ELF_CONTENT_A")
                key = archive(elf, board="esp32", version="v1.0.0",
                              archive_dir=td_store, auto_prune=False)
                # Use 9-char prefix (matches CONFIG_APP_RETRIEVE_LEN_ELF_SHA default)
                result = find(key[:9], archive_dir=td_store)
                self.assertIsNotNone(result)
                self.assertTrue(result.endswith(f"{key}.elf"))

    def test_find_returns_none_for_unknown_prefix(self):
        with tempfile.TemporaryDirectory() as td_store:
            result = find("deadbeef1", archive_dir=td_store)
            self.assertIsNone(result)

    def test_archive_idempotent_same_sha(self):
        with tempfile.TemporaryDirectory() as td_elfs:
            with tempfile.TemporaryDirectory() as td_store:
                elf = _write_elf(td_elfs, "firmware.elf", b"ELF_SAME_CONTENT")
                key1 = archive(elf, board="esp32", version="v1.0.0",
                               archive_dir=td_store, auto_prune=False)
                key2 = archive(elf, board="esp32", version="v1.0.1",
                               archive_dir=td_store, auto_prune=False)
                self.assertEqual(key1, key2)
                entries = list_entries(archive_dir=td_store)
                self.assertEqual(len(entries), 1)

    def test_sidecar_stores_board_and_version(self):
        with tempfile.TemporaryDirectory() as td_elfs:
            with tempfile.TemporaryDirectory() as td_store:
                elf = _write_elf(td_elfs, "fw.elf", b"ELF_SIDECAR_TEST")
                key = archive(elf, board="tdongle", version="v0.5.0",
                              archive_dir=td_store, auto_prune=False)
                entries = list_entries(archive_dir=td_store)
                self.assertEqual(len(entries), 1)
                meta, _ = entries[0]
                self.assertEqual(meta.board, "tdongle")
                self.assertEqual(meta.version, "v0.5.0")
                self.assertEqual(meta.sha256, key)


# ---------------------------------------------------------------------------
# auto-retention: keep-last-N-per-board
# ---------------------------------------------------------------------------

class TestAutoRetention(unittest.TestCase):

    def test_insert_n_plus_2_evicts_oldest_two(self):
        keep = 10
        with tempfile.TemporaryDirectory() as td_elfs:
            with tempfile.TemporaryDirectory() as td_store:
                # Insert 12 ELFs for boardA with deterministic timestamps.
                keys = []
                for i in range(12):
                    elf = _write_elf(td_elfs, f"fw_{i}.elf", f"ELF_CONTENT_{i}".encode())
                    ts = f"2026-01-{i+1:02d}T00:00:00Z"
                    key = archive(
                        elf, board="boardA", version=f"v{i}",
                        archive_dir=td_store, keep=keep, auto_prune=True,
                        _archived_at=ts,
                    )
                    keys.append(key)

                entries = list_entries(archive_dir=td_store)
                boardA = [m for m, _ in entries if m.board == "boardA"]
                self.assertEqual(len(boardA), keep)

                # The 2 oldest (i=0, i=1) must be gone.
                remaining_shas = {m.sha256 for m in boardA}
                self.assertNotIn(keys[0], remaining_shas)
                self.assertNotIn(keys[1], remaining_shas)
                # The 10 most-recent must survive.
                for k in keys[2:]:
                    self.assertIn(k, remaining_shas)

    def test_per_board_keeps_separate_budgets(self):
        """Pruning boardA does not affect boardB."""
        keep = 2
        with tempfile.TemporaryDirectory() as td_elfs:
            with tempfile.TemporaryDirectory() as td_store:
                for i in range(3):
                    elf = _write_elf(td_elfs, f"a_{i}.elf", f"BOARD_A_{i}".encode())
                    ts = f"2026-01-{i+1:02d}T00:00:00Z"
                    archive(elf, board="boardA", version=f"va{i}",
                            archive_dir=td_store, keep=keep, _archived_at=ts)
                for i in range(3):
                    elf = _write_elf(td_elfs, f"b_{i}.elf", f"BOARD_B_{i}".encode())
                    ts = f"2026-02-{i+1:02d}T00:00:00Z"
                    archive(elf, board="boardB", version=f"vb{i}",
                            archive_dir=td_store, keep=keep, _archived_at=ts)

                entries = list_entries(archive_dir=td_store)
                boardA = [m for m, _ in entries if m.board == "boardA"]
                boardB = [m for m, _ in entries if m.board == "boardB"]
                self.assertEqual(len(boardA), keep)
                self.assertEqual(len(boardB), keep)


# ---------------------------------------------------------------------------
# prune: in-use protection
# ---------------------------------------------------------------------------

class TestPruneInUseProtection(unittest.TestCase):

    def test_in_use_sha_survives_pruning(self):
        with tempfile.TemporaryDirectory() as td_elfs:
            with tempfile.TemporaryDirectory() as td_store:
                elf_old = _write_elf(td_elfs, "old.elf", b"ELF_OLD_CONTENT")
                elf_new = _write_elf(td_elfs, "new.elf", b"ELF_NEW_CONTENT")

                sha_old = archive(elf_old, board="esp32", version="v0.1.0",
                                  archive_dir=td_store, auto_prune=False,
                                  _archived_at="2026-01-01T00:00:00Z")
                sha_new = archive(elf_new, board="esp32", version="v0.2.0",
                                  archive_dir=td_store, auto_prune=False,
                                  _archived_at="2026-01-02T00:00:00Z")

                # Normally prune(keep=1) for board "esp32" would remove sha_old.
                # Protect sha_old via in_use_shas → nothing should be deleted.
                deleted = prune(keep=1, in_use_shas={sha_old},
                                archive_dir=td_store)
                self.assertNotIn(sha_old, deleted)
                entries = list_entries(archive_dir=td_store)
                shas = {m.sha256 for m, _ in entries}
                self.assertIn(sha_old, shas)

    def test_unprotected_sha_is_pruned(self):
        with tempfile.TemporaryDirectory() as td_elfs:
            with tempfile.TemporaryDirectory() as td_store:
                elf_old = _write_elf(td_elfs, "old.elf", b"ELF_PRUNE_OLD")
                elf_new = _write_elf(td_elfs, "new.elf", b"ELF_PRUNE_NEW")

                sha_old = archive(elf_old, board="esp32", version="v0.1.0",
                                  archive_dir=td_store, auto_prune=False,
                                  _archived_at="2026-01-01T00:00:00Z")
                archive(elf_new, board="esp32", version="v0.2.0",
                        archive_dir=td_store, auto_prune=False,
                        _archived_at="2026-01-02T00:00:00Z")

                deleted = prune(keep=1, archive_dir=td_store)
                self.assertIn(sha_old, deleted)

    def test_grace_keep_protects_globally(self):
        """grace_keep=1 keeps the most-recent entry even if keep=0."""
        with tempfile.TemporaryDirectory() as td_elfs:
            with tempfile.TemporaryDirectory() as td_store:
                elf1 = _write_elf(td_elfs, "e1.elf", b"ELF_GRACE_1")
                elf2 = _write_elf(td_elfs, "e2.elf", b"ELF_GRACE_2")
                archive(elf1, board="b1", version="v1",
                        archive_dir=td_store, auto_prune=False,
                        _archived_at="2026-01-01T00:00:00Z")
                sha2 = archive(elf2, board="b1", version="v2",
                               archive_dir=td_store, auto_prune=False,
                               _archived_at="2026-01-02T00:00:00Z")

                # max_age=-1 would delete everything; grace_keep=1 saves most-recent
                deleted = prune(keep=0, max_age=1, grace_keep=1, archive_dir=td_store)
                self.assertNotIn(sha2, deleted)


# ---------------------------------------------------------------------------
# auto-populate from esp_app_desc_t
# ---------------------------------------------------------------------------

class TestAutoPopulateFromDesc(unittest.TestCase):

    def test_archive_auto_populates_board_and_version(self):
        with tempfile.TemporaryDirectory() as td_elfs:
            with tempfile.TemporaryDirectory() as td_store:
                data = _make_esp_app_desc(version="v9.9.9",
                                          project_name="bb-smoke-esp32")
                elf = _write_elf(td_elfs, "fw.elf", data)
                key = archive(elf, archive_dir=td_store, auto_prune=False)
                entries = list_entries(archive_dir=td_store)
                meta, _ = entries[0]
                self.assertEqual(meta.version, "v9.9.9")
                self.assertEqual(meta.board, "bb-smoke-esp32")


if __name__ == "__main__":
    unittest.main()
