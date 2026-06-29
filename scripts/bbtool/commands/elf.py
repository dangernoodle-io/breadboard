"""elf command — ELF archive management (archive / list / prune)."""
from __future__ import annotations

import os
import sys

NAME = "elf"
HELP = "ELF archive management"


def add_arguments(parser) -> None:
    sub = parser.add_subparsers(dest="elf_op", metavar="OP")
    sub.required = True

    # ------------------------------------------------------------------
    # archive
    # ------------------------------------------------------------------
    p_archive = sub.add_parser("archive", help="archive a firmware ELF")
    p_archive.add_argument("elf_path", metavar="PATH", help="path to firmware .elf")
    p_archive.add_argument("--board", default="", metavar="BOARD",
                           help="board name (auto from esp_app_desc_t when omitted)")
    p_archive.add_argument("--version", default="", metavar="VER",
                           help="firmware version (auto from esp_app_desc_t when omitted)")
    p_archive.add_argument("--archive-dir", dest="archive_dir", default=None, metavar="DIR",
                           help="archive root directory (overrides config / BBTOOL_ELF_ARCHIVE)")

    # ------------------------------------------------------------------
    # list
    # ------------------------------------------------------------------
    p_list = sub.add_parser("list", help="list archived ELFs")
    p_list.add_argument("--archive-dir", dest="archive_dir", default=None, metavar="DIR",
                        help="archive root directory")
    p_list.add_argument("--hosts", default=None, metavar="H,H,…",
                        help="comma-separated IPs/hostnames to query for in-use status")

    # ------------------------------------------------------------------
    # prune
    # ------------------------------------------------------------------
    p_prune = sub.add_parser("prune", help="prune archived ELFs by budget")
    p_prune.add_argument("--keep", type=int, default=None, metavar="N",
                         help="keep N most-recent entries per board (default: from config or 10)")
    p_prune.add_argument("--max-age", dest="max_age", default=None, metavar="DUR",
                         help="delete entries older than DUR (e.g. 30d, 7d, 1h)")
    p_prune.add_argument("--in-use", dest="in_use", action="store_true",
                         help="protect ELFs running on --hosts devices from pruning")
    p_prune.add_argument("--hosts", default=None, metavar="H,H,…",
                         help="comma-separated IPs/hostnames (required with --in-use)")
    p_prune.add_argument("--grace-keep", dest="grace_keep", type=int, default=0, metavar="N",
                         help="always keep the N most-recently archived entries globally")
    p_prune.add_argument("--dry-run", action="store_true",
                         help="show what would be deleted without deleting")
    p_prune.add_argument("--yes", action="store_true",
                         help="skip confirmation prompt")
    p_prune.add_argument("--archive-dir", dest="archive_dir", default=None, metavar="DIR",
                         help="archive root directory")


def _parse_age(s: str) -> float:
    s = s.strip()
    if s.endswith("d"):
        return float(s[:-1]) * 86400
    if s.endswith("h"):
        return float(s[:-1]) * 3600
    if s.endswith("m"):
        return float(s[:-1]) * 60
    if s.endswith("s"):
        return float(s[:-1])
    return float(s)


def _query_running_shas(hosts_str: str) -> set:
    """Query /api/info (then /api/diag/panic fallback) on each host for app_sha256."""
    import json as _json
    import urllib.request
    running: set = set()
    for host in hosts_str.split(","):
        host = host.strip()
        if not host:
            continue
        sha = ""
        try:
            with urllib.request.urlopen(f"http://{host}/api/info", timeout=5) as r:
                data = _json.loads(r.read())
                sha = (data.get("build") or {}).get("app_sha256", "") or ""
        except Exception:
            pass
        if not sha:
            try:
                with urllib.request.urlopen(f"http://{host}/api/diag/panic", timeout=5) as r:
                    data = _json.loads(r.read())
                    sha = data.get("app_sha256", "") or ""
            except Exception:
                pass
        if sha:
            running.add(sha.lower())
    return running


def _elf_cfg(args) -> dict:
    return getattr(args, "_config_dict", {}).get("elf", {})


def _archive_dir(args) -> str | None:
    return getattr(args, "archive_dir", None) or _elf_cfg(args).get("archive_dir")


def cmd_archive(args) -> int:
    from elfstore import archive as elf_archive, _load_meta, _resolve_archive_root

    cfg = _elf_cfg(args)
    ad = _archive_dir(args)
    keep = int(cfg.get("keep", 10))
    auto_prune = cfg.get("auto_prune", True)

    try:
        key = elf_archive(
            args.elf_path,
            board=getattr(args, "board", ""),
            version=getattr(args, "version", ""),
            archive_dir=ad,
            keep=keep,
            auto_prune=auto_prune,
        )
        root = _resolve_archive_root(ad)
        meta = _load_meta(root / f"{key}.json")
        print(f"Archived: {args.elf_path}")
        print(f"  sha256     : {key}")
        print(f"  board      : {meta.board or '(unset)'}")
        print(f"  version    : {meta.version or '(unset)'}")
        print(f"  build_time : {meta.build_time or '(unset)'}")
        return 0
    except FileNotFoundError:
        print(f"ERROR: ELF not found: {args.elf_path}")
        return 1
    except Exception as exc:
        print(f"ERROR: archive failed: {exc}")
        return 1


def cmd_list(args) -> int:
    import datetime
    from elfstore import list_entries

    ad = _archive_dir(args)
    entries = list_entries(archive_dir=ad)
    if not entries:
        print("No archived ELFs.")
        return 0

    running_shas: set = set()
    hosts_str = getattr(args, "hosts", None)
    if hosts_str:
        try:
            running_shas = _query_running_shas(hosts_str)
        except Exception as exc:
            print(f"WARNING: host query failed: {exc}")

    print(f"\n{'SHA256 (prefix)':<20} {'BOARD':<24} {'VERSION':<18} {'ARCHIVED':<22} {'SIZE':>10}  IN-USE")
    print("-" * 100)
    for meta, size in entries:
        sha_short = meta.sha256[:16] + "…"
        size_str = f"{size:,}"
        if running_shas:
            in_use = any(meta.sha256.lower().startswith(s.lower()) for s in running_shas)
            in_use_str = "yes" if in_use else "no"
        else:
            in_use_str = "?" if hosts_str else "-"
        print(f"{sha_short:<20} {meta.board:<24} {meta.version:<18} {meta.archived_at:<22} {size_str:>10}  {in_use_str}")

    total = sum(s for _, s in entries)
    print(f"\n{len(entries)} archived ELF(s), {total:,} bytes total")
    return 0


def cmd_prune(args) -> int:
    from elfstore import list_entries, prune as elf_prune

    cfg = _elf_cfg(args)
    ad = _archive_dir(args)
    keep = getattr(args, "keep", None)
    if keep is None:
        keep = int(cfg.get("keep", 10))
    max_age_secs: float | None = None
    if getattr(args, "max_age", None):
        max_age_secs = _parse_age(args.max_age)
    grace_keep = getattr(args, "grace_keep", 0)
    dry_run = getattr(args, "dry_run", False)
    yes = getattr(args, "yes", False)
    in_use_mode = getattr(args, "in_use", False)

    protected: set = set()
    if in_use_mode:
        hosts_str = getattr(args, "hosts", None)
        if not hosts_str:
            print("ERROR: --in-use requires --hosts <H,H,…>")
            return 1
        try:
            running_shas = _query_running_shas(hosts_str)
        except Exception as exc:
            print(f"ERROR: host query failed: {exc}")
            return 1
        if not running_shas:
            print("WARNING: no running shas found; no in-use protection applied")
        else:
            # Expand short shas to full archive keys
            entries = list_entries(archive_dir=ad)
            for meta, _ in entries:
                for short in running_shas:
                    if meta.sha256.lower().startswith(short.lower()):
                        protected.add(meta.sha256)
            print(f"In-use protection: {len(running_shas)} sha(s), {len(protected)} archive entries protected")

    would_delete = elf_prune(
        keep=keep,
        max_age=max_age_secs,
        in_use_shas=protected,
        grace_keep=grace_keep,
        dry_run=True,
        archive_dir=ad,
    )
    if not would_delete:
        print("Nothing to prune.")
        return 0

    label = "[DRY-RUN] " if dry_run else ""
    print(f"{label}Would delete {len(would_delete)} entry(ies):")
    for sha in would_delete:
        print(f"  {sha[:16]}…")

    if dry_run:
        return 0

    if not yes:
        try:
            ans = input(f"Delete {len(would_delete)} entry(ies)? [y/N] ")
        except EOFError:
            ans = ""
        if ans.strip().lower() not in ("y", "yes"):
            print("Aborted.")
            return 1

    deleted = elf_prune(
        keep=keep,
        max_age=max_age_secs,
        in_use_shas=protected,
        grace_keep=grace_keep,
        dry_run=False,
        archive_dir=ad,
    )
    print(f"Deleted {len(deleted)} entry(ies).")
    return 0


def register(api) -> None:
    api.add_command(NAME, sys.modules[__name__])


def run(args) -> int:
    op = getattr(args, "elf_op", None)
    if op == "archive":
        return cmd_archive(args)
    if op == "list":
        return cmd_list(args)
    if op == "prune":
        return cmd_prune(args)
    return 1
