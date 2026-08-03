"""Shared library: config loading, Context, plugin loader, atomic file writes."""
from __future__ import annotations
import importlib.util
import os
import sys
import tempfile
import tomllib
from pathlib import Path
from typing import Generator, Iterable, Optional


def write_if_changed(path: str, content: str, *, makedirs: bool = False) -> bool:
    """Write `content` to `path`, but ONLY if it differs from what's already
    there (or `path` doesn't exist yet) -- and, when it does write, via a
    same-directory tmp file + `os.replace` so a reader never observes a
    partial file. Second hand-rolled instance of this idiom (commands/
    version.py had the first; commands/codegen.py the second) triggered
    extraction per the Consolidation convention in CLAUDE.md (B1-1403 review
    finding 3) -- both call sites now delegate here instead of maintaining
    their own copies.

    The tmp filename is UNIQUE per call (`tempfile.mkstemp`, same directory
    as `path`), not a fixed `f"{path}.tmp"` -- B1-1403 review finding 4: a
    fixed name can tear under concurrent writers (e.g. `make smoke-gen-*`
    racing a `pio run`'s own CMake-configure-time regeneration of the same
    physical `bb_app_init.c`), where one process's `os.replace` can promote
    another's still-partially-written tmp file. A unique-per-call name makes
    that interleaving impossible: each writer only ever replaces its own
    tmp file with its own content.

    On any write failure the tmp file is removed before the exception
    propagates (B1-1403 review finding 5) -- never left behind as orphaned
    generated-directory litter.

    `makedirs=True` creates `path`'s parent directory first (`exist_ok`),
    matching `commands/version.py`'s pre-extraction behavior exactly; the
    default `False` matches `commands/codegen.py`'s pre-extraction
    behavior, whose call sites already `os.makedirs` themselves before
    calling this.

    Returns True if the file was written, False if `content` was already
    byte-identical on disk (a content-stable regeneration is then a true
    no-op for the build graph -- the file's mtime is left untouched, so
    ninja/make never treats it, or anything depending on it, as changed).

    Permission bits (B1-1403 review finding 1): `os.replace(tmp_path, path)`
    promotes the TMP FILE's inode -- including its mode -- over `path`; it
    does not preserve whatever mode `path` already had. `tempfile.mkstemp`
    always creates at 0o600 regardless of umask, so without an explicit
    `os.chmod` before the replace, every regeneration of a pre-existing file
    would silently ratchet its permissions down to owner-only, breaking any
    later step (a Docker multi-stage COPY, a lint/docs job, a tar/artifact
    upload) that reads the generated file as a different UID/GID. Two cases,
    handled explicitly:
      - overwriting an existing file: the tmp file is chmod'd to that file's
        existing mode bits, so a regeneration never changes them.
      - creating a new file: the tmp file is chmod'd to a umask-respecting
        default (0o666 & ~umask), matching what `open(path, "w")` would have
        produced -- not a hardcoded 0o644, which would ignore a caller's
        more restrictive umask. `os.umask()` is the only way to read the
        current umask and is itself read-modify-write (not thread-safe); the
        umask is restored immediately after the read, in the same statement
        pair, to minimize the window."""
    existing_mode: Optional[int] = None
    if os.path.exists(path):
        try:
            with open(path, encoding="utf-8") as fh:
                if fh.read() == content:
                    return False
        except OSError:
            pass
        existing_mode = os.stat(path).st_mode & 0o777
    if makedirs:
        os.makedirs(os.path.dirname(path), exist_ok=True)
    fd, tmp_path = tempfile.mkstemp(
        dir=os.path.dirname(path) or ".", prefix=os.path.basename(path) + ".")
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            f.write(content)
        if existing_mode is not None:
            os.chmod(tmp_path, existing_mode)
        else:
            current_umask = os.umask(0)
            os.umask(current_umask)
            os.chmod(tmp_path, 0o666 & ~current_umask)
        os.replace(tmp_path, path)
    except BaseException:
        try:
            os.remove(tmp_path)
        except OSError:
            pass
        raise
    return True


def load_config(config_path: Optional[str], root: str) -> dict:
    """Load bbtool.toml. Discovery: --config → <root>/bbtool.toml → {}."""
    if config_path:
        path = Path(config_path)
    else:
        path = Path(root) / "bbtool.toml"
    if not path.exists():
        return {}
    try:
        with open(path, "rb") as fh:
            return tomllib.load(fh)
    except Exception as e:
        print(f"bbtool: failed to load config {path}: {e}", file=sys.stderr)
        return {}


class Context:
    """Shared context passed to rule check functions."""

    def __init__(self, root: str, config: dict) -> None:
        self.root = os.path.abspath(root)
        self.config = config

    def files(
        self,
        globs: Iterable[str],
        relative_to: Optional[str] = None,
        exclude_dirs: Optional[Iterable[str]] = None,
    ) -> Generator[Path, None, None]:
        """Yield paths matching any of the globs under root, skipping excluded dir names."""
        from fnmatch import fnmatch
        exclude_set = set(exclude_dirs or [])
        root = Path(self.root)
        seen = set()
        for pattern in globs:
            for path in root.glob(pattern):
                if path in seen:
                    continue
                # Check no path component is in exclude_set
                parts = path.relative_to(root).parts
                if any(p in exclude_set for p in parts):
                    continue
                seen.add(path)
                yield path

    def read(self, path: Path) -> str:
        """Read a file as UTF-8 with errors='replace'."""
        with open(path, encoding="utf-8", errors="replace") as fh:
            return fh.read()

    def violation(self, path: Path, line: int, detail: str = "") -> dict:
        return {"path": str(path), "line": line, "detail": detail}


def load_plugins(paths: Iterable[str], config_dir: str, api: object) -> None:
    """Load plugin .py files; warn on failure, continue."""
    for p in paths:
        plugin_path = Path(p) if os.path.isabs(p) else Path(config_dir) / p
        try:
            spec = importlib.util.spec_from_file_location("_bbtool_plugin", plugin_path)
            if spec is None or spec.loader is None:
                raise ImportError(f"cannot load spec from {plugin_path}")
            mod = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(mod)
            if hasattr(mod, "register"):
                mod.register(api)
        except Exception as e:
            print(f"bbtool: plugin load failed ({plugin_path}): {e}", file=sys.stderr)
