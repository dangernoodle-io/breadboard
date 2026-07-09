"""Public-header Doxygen `@brief` extraction — the ONE header annotation
channel `bbtool docs gen` reads for prose (see rules/docs.md's sourcing
model). There is no `@wiki` tag; a component's own wiki link is derived
separately from the `[docs].wiki_base` convention (see commands/docs.py's
`_self_wiki_link`).

Supports `/** ... */`, `///`, and `//!` comment styles. Deterministic,
stdlib `re` only.
"""
from __future__ import annotations
import re
from pathlib import Path
from typing import Optional

# Matches an `@brief` tag in any of the three supported comment styles,
# capturing everything up to end-of-line as the initial text. Continuation
# lines (block-comment ` * ` noise, or repeated `///`/`//!` prefixes) are
# joined in afterward by `extract_brief` until a blank line, a new `@tag`,
# or a comment terminator is reached.
_BRIEF_LINE_RE = re.compile(r'@brief\b[ \t]*(.*)')
_BLOCK_CONT_RE = re.compile(r'^[ \t]*\*[ \t]?(.*)$')
_SLASH_CONT_RE = re.compile(r'^[ \t]*(?:///|//!)[ \t]?(.*)$')
_NEW_TAG_RE = re.compile(r'^[ \t]*(?:\*[ \t]*|(?:///|//!)[ \t]*)?@\w+')
_BLOCK_END_RE = re.compile(r'\*/')


def extract_brief(path: Path) -> Optional[str]:
    """Scan `path` (a C header) for the FIRST `@brief` tag and return its
    text, joining wrapped continuation lines until a blank line, the next
    `@tag`, or a comment terminator. Returns None if the file is missing or
    has no `@brief`."""
    if not path.is_file():
        return None
    try:
        text = path.read_text(encoding="utf-8")
    except OSError:
        return None

    lines = text.splitlines()

    # Track block-comment nesting state as we scan sequentially, so an
    # `@brief` on a continuation line (not the `/**` line itself) is still
    # recognized as being inside a block comment.
    block_open = False
    for i, line in enumerate(lines):
        line_has_open = "/**" in line
        if line_has_open:
            open_idx = line.index("/**")
            close_after_open = "*/" in line[open_idx:]
        else:
            close_after_open = False

        m = _BRIEF_LINE_RE.search(line)
        if not m:
            if line_has_open:
                block_open = not close_after_open
            elif block_open and "*/" in line:
                block_open = False
            continue

        in_block = block_open or (line_has_open and not close_after_open)
        raw = m.group(1)
        close_idx = raw.find("*/")
        if close_idx != -1:
            raw = raw[:close_idx]
        parts = [raw.strip()]
        j = i + 1
        while j < len(lines):
            nxt = lines[j]
            if not nxt.strip():
                break
            if _NEW_TAG_RE.match(nxt):
                break
            if in_block:
                if _BLOCK_END_RE.search(nxt):
                    cont = nxt.split("*/", 1)[0]
                    cm = _BLOCK_CONT_RE.match(cont)
                    text_part = cm.group(1).strip() if cm else cont.strip()
                    if text_part:
                        parts.append(text_part)
                    break
                cm = _BLOCK_CONT_RE.match(nxt)
                if cm is None:
                    break
                text_part = cm.group(1).strip()
                if not text_part:
                    break
                parts.append(text_part)
            else:
                sm = _SLASH_CONT_RE.match(nxt)
                if sm is None:
                    break
                text_part = sm.group(1).strip()
                if not text_part:
                    break
                parts.append(text_part)
            j += 1

        brief = " ".join(p for p in parts if p).strip()
        return brief if brief else None

    return None


def primary_header(root: Path, name: str) -> Path:
    """Return the conventional primary public header for component `name`:
    components/<name>/include/<name>.h."""
    return root / "components" / name / "include" / f"{name}.h"
