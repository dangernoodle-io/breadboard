"""lint command — rules-based source linter."""
from __future__ import annotations
import argparse
import configparser
import os
import re
import sys
from pathlib import Path
from typing import List, Optional

from core import Context
from registry import Rule, RULES
from cmake_parse import (
    parse_requires,
    parse_paths,
    parse_embed_assets,
    parse_target_include_directories,
    parse_include_calls,
    single_opaque_property_vars,
    strip_cmake_comments,
    ConditionalSetError,
)
from boards import discover_components, ManifestError
from composition import resolve_composition
from discovery import build_index, normalize_roots

NAME = "lint"
HELP = "Run source lint checks"


# ---------------------------------------------------------------------------
# Discovery-SSOT helpers shared by every rule that used to hand-roll a
# `comp_root.glob("*/include/*.h")` / `comp_root.glob("*/CMakeLists.txt")`
# single-level walk (B1-1084 consumer migration). Both are depth-agnostic —
# a nested component is found via `discovery.build_index` rather than a
# glob shape that silently matches zero paths once a component moves off
# `components/<name>/` (a SILENT-omission bug, not merely a stale path).
# ---------------------------------------------------------------------------

def _public_headers(ctx: Context, exclude: frozenset = frozenset()) -> List[Path]:
    """Every public header (`include/*.h`) of every discovered component,
    sorted for determinism. Byte-identical ordering to the old
    `sorted(comp_root.glob("*/include/*.h"))` on today's flat tree (both are
    a path-string sort with an identical `components/<name>/include/`
    prefix per component)."""
    index = build_index(normalize_roots(str(Path(ctx.root))))
    headers: List[Path] = []
    for name in index.names():
        if name in exclude:
            continue
        comp_dir = index.component_dir(name)
        if comp_dir is None:
            continue
        inc_dir = comp_dir / "include"
        if inc_dir.is_dir():
            headers.extend(inc_dir.glob("*.h"))
    return sorted(headers)


def _component_cmake_files(ctx: Context) -> List[Path]:
    """Every discovered component's own `CMakeLists.txt` (components-layer
    only, mirroring the old `comp_root.glob("*/CMakeLists.txt")`'s scope),
    sorted by path for determinism — byte-identical ordering to the old glob
    on today's flat tree."""
    index = build_index(normalize_roots(str(Path(ctx.root))))
    files: List[Path] = []
    for name in index.names():
        comp_dir = index.component_dir(name)
        if comp_dir is None:
            continue
        cmake_file = comp_dir / "CMakeLists.txt"
        if cmake_file.is_file():
            files.append(cmake_file)
    return sorted(files)


_BB_DISPLAY_EK79007_EXCLUDE = frozenset({"bb_display_ek79007"})

# ---------------------------------------------------------------------------
# Rule implementations (4 ported from check_lint.sh)
# ---------------------------------------------------------------------------

def _check_deprecated_http_send(ctx: Context) -> list:
    """Rule: deprecated-http-send — flags bb_http_resp_send_json/err/send calls."""
    violations = []
    pattern = re.compile(
        r'bb_http_resp_send_json\(|bb_http_resp_send_err\(|bb_http_resp_send\('
    )
    exclude_pattern = re.compile(r'bb_http_resp_send_chunk|bb_http_resp_sendstr')
    root = Path(ctx.root)
    comp_root = root / "components"
    if not comp_root.exists():
        return violations
    for path in ctx.files(
        ["components/**/*.c", "components/**/*.h"],
        exclude_dirs=[".pio", ".claude"],
    ):
        content = ctx.read(path)
        for i, line in enumerate(content.splitlines(), 1):
            if pattern.search(line) and not exclude_pattern.search(line):
                violations.append(ctx.violation(path, i))
    return violations


def _check_public_header_leak(ctx: Context) -> list:
    """Rule: public-header-leak — flags ungated esp_/driver/cJSON includes in public headers."""
    violations = []

    include_pattern = re.compile(
        r'["<](esp_|freertos/|lwip/|mbedtls/)'
        r'|["<]driver/'
        r'|["<]cJSON\.h'
        r'|["<]mdns\.h'
        r'|["<]nvs(?:_flash)?\.h'
        r'|["<]httpd_'
        r'|["<]sdkconfig\.h'
        r'|["<]pthread\.h'
    )
    ifdef_open = re.compile(
        r'^\s*#\s*(?:ifdef\s+ESP_PLATFORM|if\s+defined\s*\(\s*ESP_PLATFORM\s*\))'
    )
    elif_else = re.compile(r'^\s*#\s*(?:elif|else)\b')
    endif_re = re.compile(r'^\s*#\s*endif\b')
    include_re = re.compile(r'^\s*#\s*include\b')

    # Find all public headers, excluding bb_display_ek79007
    for path in _public_headers(ctx, exclude=_BB_DISPLAY_EK79007_EXCLUDE):
        content = ctx.read(path)
        gate = 0
        for i, line in enumerate(content.splitlines(), 1):
            if ifdef_open.match(line):
                gate += 1
                continue
            if elif_else.match(line):
                if gate > 0:
                    gate -= 1
                continue
            if endif_re.match(line):
                if gate > 0:
                    gate -= 1
                continue
            if include_re.match(line):
                if gate == 0 and include_pattern.search(line):
                    violations.append(ctx.violation(path, i, line.strip()))
    return violations


def _check_state_topic_post(ctx: Context) -> list:
    """Rule: state-topic-post — flags direct bb_event_post of state topics outside bb_cache."""
    violations = []
    root = Path(ctx.root)

    topic_pattern = re.compile(
        r'BB_NET_HEALTH_TOPIC|BB_DIAG_BOOT_TOPIC|BB_OTA_CHECK_TOPIC|BB_DISPLAY_INFO_TOPIC'
        r'|"net\.health"|"diag\.boot"|"update\.available"|"health\.display"'
    )
    post_pattern = re.compile(r'bb_event_post\(')

    # Excluded dirs (use Path.is_relative_to)
    excluded_dirs = [
        root / "platform" / "espidf" / "bb_cache",
        root / "platform" / "host" / "bb_cache",
        root / "components" / "bb_cache",
        root / "test",
    ]

    for path in ctx.files(
        ["**/*.c", "**/*.h"],
        exclude_dirs=[".pio", ".claude"],
    ):
        # Check if path is under any excluded dir
        skip = False
        for excl in excluded_dirs:
            try:
                path.relative_to(excl)
                skip = True
                break
            except ValueError:
                pass
        if skip:
            continue

        content = ctx.read(path)
        for i, line in enumerate(content.splitlines(), 1):
            if post_pattern.search(line) and topic_pattern.search(line):
                violations.append(ctx.violation(path, i))
    return violations


# ---------------------------------------------------------------------------
# Rule: raw-allocator
# ---------------------------------------------------------------------------

def _check_raw_allocator(ctx: Context) -> list:
    """Rule: raw-allocator — flags raw malloc/calloc/free outside the bb_mem facade."""
    violations = []
    pattern = re.compile(
        r'\bmalloc\(|\bcalloc\(|\brealloc\(|\bfree\('
        r'|\bheap_caps_malloc\(|\bheap_caps_calloc\('
        r'|\bheap_caps_realloc\(|\bheap_caps_free\('
        r'|\bheap_caps_aligned_alloc\('
    )
    root = Path(ctx.root)

    rule_cfg = ctx.config.get("lint", {}).get("rules", {}).get(
        "raw-allocator", {}
    )
    allowlist: set = set(rule_cfg.get("allow", []))

    for path in ctx.files(
        ["platform/espidf/**/*.c", "platform/espidf/**/*.h",
         "components/**/*.c", "components/**/*.h"],
        exclude_dirs=[".pio", ".claude"],
    ):
        # Skip test directories
        parts = path.relative_to(root).parts
        if "test" in parts:
            continue
        # Exempt bb_mem.c — the facade implementation
        if path.name == "bb_mem.c":
            continue
        # Check path-level allowlist entry
        rel = str(path.relative_to(root))
        if rel in allowlist:
            continue

        content = ctx.read(path)
        # Strip comments and string literals before scanning so we don't fire on
        # "malloc(" in a log string or a // free(p) comment.
        stripped = _strip_noise(content)

        for i, line in enumerate(stripped.splitlines(), 1):
            if not pattern.search(line):
                continue
            # Check path:line allowlist entry
            key = f"{rel}:{i}"
            if key in allowlist:
                continue
            violations.append(ctx.violation(path, i))
    return violations


# ---------------------------------------------------------------------------
# Rule: raw-esp-timer
# ---------------------------------------------------------------------------

def _check_raw_esp_timer(ctx: Context) -> list:
    """Rule: raw-esp-timer — flags esp_timer_create/_create_args_t outside bb_timer/."""
    violations = []
    pattern = re.compile(r'\besp_timer_create\b|\besp_timer_create_args_t\b')
    root = Path(ctx.root)
    bb_timer_dir = root / "platform" / "espidf" / "bb_timer"

    for path in ctx.files(
        ["platform/**/*.c", "platform/**/*.h",
         "components/**/*.c", "components/**/*.h"],
        exclude_dirs=[".pio", ".claude"],
    ):
        # Skip test fixtures
        parts = path.relative_to(root).parts
        if "test" in parts:
            continue
        # Exempt bb_timer directory
        try:
            path.relative_to(bb_timer_dir)
            continue
        except ValueError:
            pass
        content = ctx.read(path)
        for i, line in enumerate(content.splitlines(), 1):
            if pattern.search(line):
                violations.append(ctx.violation(path, i))
    return violations


# ---------------------------------------------------------------------------
# Rule: timer-cb-heavy
# ---------------------------------------------------------------------------

_TIMER_CB_KEYWORDS = frozenset({
    "void", "int", "char", "const", "unsigned", "signed", "struct", "enum",
    "typedef", "static", "inline", "extern", "return", "if", "else", "for",
    "while", "do", "switch", "case", "break", "NULL", "true", "false",
})

_TIMER_REG_RE = re.compile(
    r'\bbb_timer_(?:periodic|oneshot)_create\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)'
)

_HEAVY_PATTERNS = [
    re.compile(r'\bxSemaphoreTake\s*\([^)]*portMAX_DELAY'),
    re.compile(r'\bmalloc\b'),
    re.compile(r'\bbb_json_'),
    re.compile(r'\bbb_cache_'),
    re.compile(r'\bmdns_'),
    re.compile(r'\besp_wifi_'),
    re.compile(r'\besp_restart\b'),
    re.compile(r'\bxTaskCreate\b'),
    re.compile(r'\bbb_event_post\b'),
]


def _strip_noise(src: str) -> str:
    """Single-pass: blank string literals, char literals, // and /* */ comments.

    Preserves all newlines and character offsets so line numbers stay accurate.
    """
    out = []
    i = 0
    n = len(src)
    while i < n:
        c = src[i]
        # Line comment
        if c == '/' and i + 1 < n and src[i + 1] == '/':
            out.append(' ')
            out.append(' ')
            i += 2
            while i < n and src[i] != '\n':
                out.append(' ')
                i += 1
        # Block comment
        elif c == '/' and i + 1 < n and src[i + 1] == '*':
            out.append(' ')
            out.append(' ')
            i += 2
            while i < n:
                if src[i] == '*' and i + 1 < n and src[i + 1] == '/':
                    out.append(' ')
                    out.append(' ')
                    i += 2
                    break
                out.append('\n' if src[i] == '\n' else ' ')
                i += 1
        # String literal
        elif c == '"':
            out.append(' ')
            i += 1
            while i < n and src[i] != '"':
                if src[i] == '\\' and i + 1 < n:
                    out.append(' ')
                    out.append('\n' if src[i + 1] == '\n' else ' ')
                    i += 2
                else:
                    out.append('\n' if src[i] == '\n' else ' ')
                    i += 1
            if i < n:
                out.append(' ')
                i += 1
        # Char literal
        elif c == "'":
            out.append(' ')
            i += 1
            while i < n and src[i] != "'":
                if src[i] == '\\' and i + 1 < n:
                    out.append(' ')
                    out.append('\n' if src[i + 1] == '\n' else ' ')
                    i += 2
                else:
                    out.append('\n' if src[i] == '\n' else ' ')
                    i += 1
            if i < n:
                out.append(' ')
                i += 1
        else:
            out.append(c)
            i += 1
    return ''.join(out)


def _walk_balanced(text: str, start: int, open_ch: str, close_ch: str) -> int:
    """Starting AT the open_ch at text[start], walk to matching close_ch.
    Returns index of the closing char, or -1 if not found.
    """
    assert text[start] == open_ch
    depth = 0
    i = start
    n = len(text)
    while i < n:
        if text[i] == open_ch:
            depth += 1
        elif text[i] == close_ch:
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return -1


def _find_cb_body(stripped: str, cb_name: str) -> Optional[str]:
    """Locate the DEFINITION of cb_name in stripped text.

    A definition: cb_name as a word, followed (after optional ws/parens) by `{`.
    Returns the body (between the braces, exclusive), or None if not found.
    """
    word_re = re.compile(r'\b' + re.escape(cb_name) + r'\b')
    n = len(stripped)
    for m in word_re.finditer(stripped):
        pos = m.end()
        # Skip whitespace
        while pos < n and stripped[pos] in ' \t\r\n':
            pos += 1
        if pos >= n or stripped[pos] != '(':
            continue
        # Walk param list
        close_paren = _walk_balanced(stripped, pos, '(', ')')
        if close_paren < 0:
            continue
        pos = close_paren + 1
        # Skip whitespace
        while pos < n and stripped[pos] in ' \t\r\n':
            pos += 1
        if pos >= n or stripped[pos] != '{':
            continue
        # This is a definition — walk the body
        close_brace = _walk_balanced(stripped, pos, '{', '}')
        if close_brace < 0:
            continue
        return stripped[pos + 1:close_brace]
    return None


def _check_timer_cb_heavy(ctx: Context) -> list:
    """Rule: timer-cb-heavy — flags heavy work in bb_timer_(periodic|oneshot)_create callbacks."""
    violations = []
    root = Path(ctx.root)

    for path in ctx.files(
        ["platform/**/*.c", "platform/**/*.h",
         "components/**/*.c", "components/**/*.h"],
        exclude_dirs=[".pio", ".claude"],
    ):
        parts = path.relative_to(root).parts
        if "test" in parts:
            continue

        src = ctx.read(path)
        stripped = _strip_noise(src)
        lines = stripped.splitlines()

        # Find all bb_timer_(periodic|oneshot)_create( first-arg-ident ) registrations
        for m in _TIMER_REG_RE.finditer(stripped):
            cb_name = m.group(1)
            if cb_name in _TIMER_CB_KEYWORDS:
                continue

            # Line number of the registration
            reg_line = stripped[:m.start()].count('\n') + 1

            # Find callback definition body
            body = _find_cb_body(stripped, cb_name)
            if body is None:
                # Callback defined elsewhere — accepted gap
                continue

            # Scan body for heavy tokens
            for hp in _HEAVY_PATTERNS:
                hm = hp.search(body)
                if hm:
                    token = body[hm.start():hm.end()].strip()
                    violations.append(ctx.violation(
                        path, reg_line,
                        f"callback '{cb_name}' does heavy work: {token}",
                    ))
                    break  # one violation per registration

    return violations


# ---------------------------------------------------------------------------
# Rule: platform-error-in-public-struct (B1-366)
# ---------------------------------------------------------------------------

# Integer scalar types that constitute a "raw platform error code" when
# paired with a suspicious field name / comment.
_PLAT_ERR_INT_TYPE_RE = re.compile(
    r'\b(?:int|unsigned|signed|short|long|'
    r'int8_t|int16_t|int32_t|int64_t|'
    r'uint8_t|uint16_t|uint32_t|uint64_t)\b'
)

# Field name or trailing comment matches any of these → suspicious
_PLAT_ERR_NAME_RE = re.compile(
    r'esp_err'
    r'|mbedtls'
    r'|\btls\b.{0,30}(?:err|code|fail)'
    r'|err(?:or)?_?code'
    r'|disc(?:onnect)?_?(?:reason|err)'
    r'|_errno',
    re.IGNORECASE,
)

# Struct opening patterns
_STRUCT_TYPEDEF_OPEN = re.compile(r'^\s*typedef\s+struct\b')
_STRUCT_NAMED_OPEN = re.compile(r'^\s*struct\s+\w+\s*\{')


def _check_platform_error_in_public_struct(ctx: Context) -> list:
    """Rule: platform-error-in-public-struct — flags integer struct fields that surface
    raw platform error codes (esp_err, mbedtls, tls_error_code, disc_reason…)."""
    violations = []

    # Read allowlist from config
    rule_cfg = ctx.config.get("lint", {}).get("rules", {}).get(
        "platform-error-in-public-struct", {}
    )
    allowlist: set = set(rule_cfg.get("allow", []))

    for path in _public_headers(ctx, exclude=_BB_DISPLAY_EK79007_EXCLUDE):
        content = ctx.read(path)
        lines = content.splitlines()
        in_struct = False
        brace_depth = 0

        for i, line in enumerate(lines, 1):
            stripped = line.strip()

            # Detect struct opening
            if not in_struct:
                if _STRUCT_TYPEDEF_OPEN.match(line) or _STRUCT_NAMED_OPEN.match(line):
                    in_struct = True
                    brace_depth = line.count('{') - line.count('}')
                    continue

            if in_struct:
                brace_depth += line.count('{') - line.count('}')
                if brace_depth <= 0:
                    in_struct = False
                    brace_depth = 0
                    continue

                # Must be an integer-typed declaration
                if not _PLAT_ERR_INT_TYPE_RE.search(stripped):
                    continue

                # Extract field name: last bare word before ; or [
                field_name_m = re.search(r'\b(\w+)\s*(?:\[.*?\])?\s*;', stripped)
                if field_name_m is None:
                    continue
                field_name = field_name_m.group(1)

                # Extract trailing comment (// ... or /* ... */)
                trail_m = re.search(r'(?://|/\*)(.+)', stripped)
                trail = trail_m.group(1) if trail_m else ""

                # Check field name or trailing comment
                target = field_name + " " + trail
                if _PLAT_ERR_NAME_RE.search(target):
                    # Check allowlist
                    key = f"{path}:{i}"
                    if key in allowlist or field_name in allowlist:
                        continue
                    violations.append(ctx.violation(path, i, stripped))

    return violations


# ---------------------------------------------------------------------------
# Rule: ticket-ref-in-log
# ---------------------------------------------------------------------------

def _check_ticket_ref_in_log(ctx: Context) -> list:
    """Rule: ticket-ref-in-log — flags ticket IDs inside bb_log_* string literals."""
    violations = []
    root = Path(ctx.root)

    # Read configurable prefix list
    rule_cfg = ctx.config.get("lint", {}).get("rules", {}).get(
        "ticket-ref-in-log", {}
    )
    prefixes = rule_cfg.get("prefixes", ["B1", "TA"])
    prefix_alt = "|".join(re.escape(p) for p in prefixes)
    ticket_re = re.compile(r'\b(?:' + prefix_alt + r')-\d+\b')

    # Matches bb_log_<alpha>( — the call start
    log_call_re = re.compile(r'\bbb_log_[a-z]+\s*\(')

    for path in ctx.files(
        ["platform/**/*.c", "platform/**/*.h",
         "components/**/*.c", "components/**/*.h"],
        exclude_dirs=[".pio", ".claude"],
    ):
        parts = path.relative_to(root).parts
        if "test" in parts:
            continue

        src = ctx.read(path)
        # Strip comments so we only see code + string literals
        stripped = _strip_noise(src)
        lines = stripped.splitlines()
        orig_lines = src.splitlines()

        for i, line in enumerate(lines, 1):
            if not log_call_re.search(line):
                continue
            # The ticket must appear inside the string content of the log call.
            # _strip_noise blanked all string content — so if ticket_re fires on
            # the stripped line it's NOT in a string.  We check the original line.
            orig = orig_lines[i - 1] if i - 1 < len(orig_lines) else ""
            # Only flag if the original has a log call AND a ticket ref inside
            # a double-quoted literal on the same line.
            if not log_call_re.search(orig):
                continue
            # Find all double-quoted strings on the original line
            for str_m in re.finditer(r'"([^"\\]*(?:\\.[^"\\]*)*)"', orig):
                if ticket_re.search(str_m.group(1)):
                    violations.append(ctx.violation(path, i, orig.strip()))
                    break

    return violations


# ---------------------------------------------------------------------------
# Rule: bb-prefix
# ---------------------------------------------------------------------------

# Function declarations at column 0: return-type word(s) then a name not starting with bb_
# Conservative: match only lowercase-starting bare identifiers (not ALL_CAPS macros here,
# those are handled by the macro branch below).
_BB_PREFIX_FN_RE = re.compile(
    r'^([A-Za-z_]\w*(?:\s*\*)?(?:\s+\w+)*\s+)'  # return type (may include pointer/qualifier)
    r'([a-z][a-zA-Z0-9_]*)'                       # function name (lowercase-start)
    r'\s*\('                                       # opening paren
)
_BB_PREFIX_SKIP_KW = frozenset({
    'static', 'inline', 'extern', 'typedef', 'struct', 'enum', 'union',
    'if', 'while', 'for', 'return', 'const', 'volatile', 'register',
})

# Macros: #define NAME — uppercase-start, not starting with BB_, not a header guard
_BB_PREFIX_MACRO_RE = re.compile(r'^#define\s+([A-Z][A-Z0-9_]*)\b')
_BB_PREFIX_HGUARD_RE = re.compile(r'[A-Z0-9_]+_H(?:_)?$')


def _check_bb_prefix(ctx: Context) -> list:
    """Rule: bb-prefix — flags public symbols in components/*/include/*.h not prefixed bb_/BB_."""
    violations = []

    rule_cfg = ctx.config.get("lint", {}).get("rules", {}).get("bb-prefix", {})
    allowlist: set = set(rule_cfg.get("allow", []))

    for path in _public_headers(ctx, exclude=_BB_DISPLAY_EK79007_EXCLUDE):
        content = ctx.read(path)
        for i, line in enumerate(content.splitlines(), 1):
            # --- function declarations ---
            m = _BB_PREFIX_FN_RE.match(line)
            if m:
                prefix_words = m.group(1).split()
                name = m.group(2)
                # skip if any keyword present in return-type tokens
                if not any(w in _BB_PREFIX_SKIP_KW for w in prefix_words):
                    if not name.startswith('bb_') and name not in allowlist:
                        violations.append(ctx.violation(path, i, f"function '{name}'"))
                        continue  # don't double-report same line

            # --- macro definitions ---
            mm = _BB_PREFIX_MACRO_RE.match(line)
            if mm:
                name = mm.group(1)
                if _BB_PREFIX_HGUARD_RE.search(name):
                    continue  # skip header guards
                if name.startswith('BB_'):
                    continue
                if name in allowlist:
                    continue
                violations.append(ctx.violation(path, i, f"macro '{name}'"))

    return violations


# ---------------------------------------------------------------------------
# Rule: pragma-once
# ---------------------------------------------------------------------------

def _check_pragma_once(ctx: Context) -> list:
    """Rule: pragma-once — flags public headers lacking a #pragma once line."""
    violations = []

    pragma_re = re.compile(r'^\s*#\s*pragma\s+once\b')

    for path in _public_headers(ctx, exclude=_BB_DISPLAY_EK79007_EXCLUDE):
        content = ctx.read(path)
        has_pragma = any(pragma_re.match(line) for line in content.splitlines())
        if not has_pragma:
            violations.append(ctx.violation(path, 1, "missing #pragma once"))

    return violations


# ---------------------------------------------------------------------------
# Rule: no-arduino-string
# ---------------------------------------------------------------------------

_ARDUINO_STRING_RE = re.compile(r'\bString\b')


def _check_no_arduino_string(ctx: Context) -> list:
    """Rule: no-arduino-string — flags Arduino String type usage in library sources."""
    violations = []
    root = Path(ctx.root)

    for path in ctx.files(
        ["platform/**/*.c", "platform/**/*.cpp", "platform/**/*.h",
         "components/**/*.c", "components/**/*.cpp", "components/**/*.h"],
        exclude_dirs=[".pio", ".claude"],
    ):
        parts = path.relative_to(root).parts
        if "test" in parts:
            continue

        src = ctx.read(path)
        stripped = _strip_noise(src)
        for i, line in enumerate(stripped.splitlines(), 1):
            if _ARDUINO_STRING_RE.search(line):
                violations.append(ctx.violation(path, i))

    return violations


# ---------------------------------------------------------------------------
# Rule: public-header-inline-platform-call
# ---------------------------------------------------------------------------

_INLINE_DECL_RE = re.compile(r'\b(?:static\s+)?inline\b')
_PLATFORM_CALL_RE = re.compile(r'\besp_[a-z0-9_]+\s*\(')


def _check_public_header_inline_platform_call(ctx: Context) -> list:
    """Rule: public-header-inline-platform-call — flags inline function bodies in public
    headers that call platform APIs (esp_*).  They leak the dep into every consumer's TU."""
    violations = []

    for path in _public_headers(ctx, exclude=_BB_DISPLAY_EK79007_EXCLUDE):
        src = ctx.read(path)
        stripped = _strip_noise(src)
        n = len(stripped)

        for m in _INLINE_DECL_RE.finditer(stripped):
            pos = m.end()

            # Scan forward to the first '(' (parameter list) — skip if we hit ';' first
            while pos < n and stripped[pos] not in ('(', ';'):
                pos += 1
            if pos >= n or stripped[pos] == ';':
                continue  # forward declaration, not a definition

            # Walk parameter list
            close_paren = _walk_balanced(stripped, pos, '(', ')')
            if close_paren < 0:
                continue

            pos = close_paren + 1

            # Skip whitespace
            while pos < n and stripped[pos] in ' \t\r\n':
                pos += 1

            if pos >= n or stripped[pos] != '{':
                continue  # not a function body (e.g. macro, or attribute list)

            # Walk the body
            close_brace = _walk_balanced(stripped, pos, '{', '}')
            if close_brace < 0:
                continue

            body = stripped[pos + 1:close_brace]

            pm = _PLATFORM_CALL_RE.search(body)
            if pm:
                line_no = stripped[:m.start()].count('\n') + 1
                token = pm.group(0).rstrip('(').strip()
                violations.append(ctx.violation(
                    path, line_no,
                    f"inline body calls platform API: {token}()"
                    " — de-inline into the platform impl",
                ))

    return violations


# ---------------------------------------------------------------------------
# Rule: mutating-route-needs-body-schema (B1-413)
# ---------------------------------------------------------------------------

# Matches bb_route_t variable initializer opening (not pointer/array declarations)
_ROUTE_INIT_RE = re.compile(r'\bbb_route_t\b[^;{}]*=\s*\{')

_METHOD_MUTATING_RE = re.compile(
    r'\.method\s*=\s*(BB_HTTP_POST|BB_HTTP_PATCH|BB_HTTP_PUT|BB_HTTP_DELETE)\b'
)
_CT_BODIED_RE = re.compile(
    r'\.request_content_type\s*=\s*"([^"]*)"',
    re.IGNORECASE,
)
_SCHEMA_FIELD_RE = re.compile(r'\.request_schema\s*=')
_SCHEMA_NULL_RE = re.compile(r'\.request_schema\s*=\s*NULL\b')
_SCHEMA_LITERAL_START_RE = re.compile(r'\.request_schema\s*=\s*"')
_SCHEMA_VAR_RE = re.compile(r'\.request_schema\s*=\s*([A-Za-z_]\w*)\b')


def _check_mutating_route_needs_body_schema(ctx: Context) -> list:
    """Rule: mutating-route-needs-body-schema — flag POST/PATCH/PUT routes whose
    request_content_type indicates a JSON/form body but whose request_schema is
    absent, NULL, or a bare {"type":"object"} with no properties."""
    violations = []
    root = Path(ctx.root)

    for path in ctx.files(
        ["platform/**/*.c", "platform/**/*.h",
         "components/**/*.c", "components/**/*.h"],
        exclude_dirs=[".pio", ".claude"],
    ):
        parts = path.relative_to(root).parts
        if "test" in parts:
            continue

        src = ctx.read(path)
        stripped = _strip_noise(src)
        n = len(stripped)

        for m in _ROUTE_INIT_RE.finditer(stripped):
            # Find the opening brace position
            brace_pos = stripped.rfind('{', m.start(), m.end())
            if brace_pos < 0:
                continue
            close = _walk_balanced(stripped, brace_pos, '{', '}')
            if close < 0:
                continue

            block_stripped = stripped[brace_pos:close + 1]
            block_orig = src[brace_pos:close + 1]

            # Only flag mutating methods
            if not _METHOD_MUTATING_RE.search(block_orig):
                continue

            # Only flag routes with a text body (JSON or form-urlencoded).
            # Binary uploads (octet-stream) intentionally omit a JSON schema.
            ct_m = _CT_BODIED_RE.search(block_orig)
            if not ct_m:
                continue  # no content_type = intentional bodyless action
            ct = ct_m.group(1).lower()
            if 'json' not in ct and 'urlencoded' not in ct:
                continue  # binary or other — skip

            line_no = src[:m.start()].count('\n') + 1

            # Schema explicitly NULL
            if _SCHEMA_NULL_RE.search(block_stripped):
                violations.append(ctx.violation(
                    path, line_no,
                    "mutating route with body has .request_schema = NULL"))
                continue

            # Schema field absent entirely
            if not _SCHEMA_FIELD_RE.search(block_stripped):
                violations.append(ctx.violation(
                    path, line_no,
                    "mutating route with body is missing .request_schema field"))
                continue

            # Schema is a variable reference — trust it (can't inspect statically)
            var_m = _SCHEMA_VAR_RE.search(block_orig)
            if var_m and var_m.group(1) not in ('NULL',):
                continue

            # Schema is a string literal — check for "properties"
            if _SCHEMA_LITERAL_START_RE.search(block_orig):
                schema_field_m = re.search(r'\.request_schema\s*=\s*', block_orig)
                if schema_field_m:
                    rest = block_orig[schema_field_m.end():]
                    # Schema value ends at next field assignment or closing brace
                    next_field_m = re.search(r',\s*\.', rest)
                    schema_val = rest[:next_field_m.start()] if next_field_m else rest
                    if 'properties' not in schema_val:
                        violations.append(ctx.violation(
                            path, line_no,
                            'mutating route schema is a bare {"type":"object"} with no properties'))

    return violations


# ---------------------------------------------------------------------------
# Rule: event-topic-needs-schema (B1-413)
# ---------------------------------------------------------------------------

# Matches bb_event_routes_attach / _attach_ex / _attach_ex2 — first arg
_ATTACH_CALL_RE = re.compile(
    r'\bbb_event_routes_attach(?:_ex2?)?\s*\(\s*'
    r'("(?:[^"\\]|\\.)*"|[A-Z][A-Z0-9_]+)'  # string literal OR ALL_CAPS macro
)

# bb_openapi_register_topic_schema(topic, schema, component_name) — first arg is sse_topic
_REGISTER_TOPIC_SCHEMA_RE = re.compile(
    r'\bbb_openapi_register_topic_schema\s*\(\s*'
    r'("(?:[^"\\]|\\.)*"|[A-Z][A-Z0-9_]+)'  # string literal OR ALL_CAPS macro
)

# bb_openapi_register_schema(component_name, schema, sse_topic) — third arg is sse_topic
_REGISTER_SCHEMA_RE = re.compile(
    r'\bbb_openapi_register_schema\s*\(\s*'
    r'(?:"(?:[^"\\]|\\.)*"|[A-Za-z_]\w*)\s*,\s*'   # component_name (skip)
    r'(?:"(?:[^"\\]|\\.)*"|[A-Za-z_]\w*)\s*,\s*'   # schema_literal (skip)
    r'("(?:[^"\\]|\\.)*"|[A-Z][A-Z0-9_]+|NULL\b)'  # sse_topic (capture)
)


def _check_event_topic_needs_schema(ctx: Context) -> list:
    """Rule: event-topic-needs-schema — every topic attached via
    bb_event_routes_attach* must have a schema registered via
    bb_openapi_register_topic_schema or bb_openapi_register_schema(sse_topic!=NULL).

    Cross-file two-pass: collect attached topics and registered schema topics
    across the entire scanned tree, then flag attached-but-unregistered."""
    root = Path(ctx.root)

    attached: list[tuple[str, object, int]] = []  # (token, path, line_no)
    schema_tokens: set[str] = set()

    for path in ctx.files(
        ["platform/**/*.c", "platform/**/*.h",
         "components/**/*.c", "components/**/*.h"],
        exclude_dirs=[".pio", ".claude"],
    ):
        parts = path.relative_to(root).parts
        if "test" in parts:
            continue

        src = ctx.read(path)
        lines = src.splitlines()

        # Collect attach calls (string literals or ALL_CAPS macros only)
        for m in _ATTACH_CALL_RE.finditer(src):
            token = m.group(1)
            line_no = src[:m.start()].count('\n') + 1
            attached.append((token, path, line_no))

        # Collect topic schema registrations
        for m in _REGISTER_TOPIC_SCHEMA_RE.finditer(src):
            schema_tokens.add(m.group(1))

        for m in _REGISTER_SCHEMA_RE.finditer(src):
            tok = m.group(1)
            if tok != 'NULL':
                schema_tokens.add(tok)

    violations = []
    for token, path, line_no in attached:
        if token not in schema_tokens:
            violations.append(ctx.violation(
                path, line_no,
                f"SSE topic {token} has no bb_openapi schema registration"))

    return violations


# ---------------------------------------------------------------------------
# Rule: kconfig-default-mismatch (B1-459 regression guard)
# ---------------------------------------------------------------------------

_KCONFIG_BLOCK_START_RE = re.compile(r'^config\s+(\w+)\s*$', re.MULTILINE)
_KCONFIG_INT_TYPE_RE = re.compile(r'^\s*int\b', re.MULTILINE)
_KCONFIG_DEFAULT_RE = re.compile(
    r'^\s*default\s+(-?\d+)(\s+if\s+[^\n]+)?\s*$', re.MULTILINE
)
_C_IFNDEF_BB_RE = re.compile(r'^\s*#ifndef\s+(BB_[A-Z0-9_]+)\s*$')


def _parse_kconfig_int_defaults(text: str) -> dict:
    """Return {config_name: base_default_int} for int-typed `config NAME`
    blocks. The "base" default is the one WITHOUT an `if <gate>` condition —
    gate-keyed defaults (e.g. `default 1024 if SPIRAM` / `default 512`) are
    skipped in favor of the ungated fallback line. Real Kconfig semantics
    mean the FIRST ungated default wins (top-to-bottom); if a (malformed)
    block has more than one ungated default, later ones are ignored."""
    result: dict[str, int] = {}
    starts = list(_KCONFIG_BLOCK_START_RE.finditer(text))
    for idx, m in enumerate(starts):
        name = m.group(1)
        block_start = m.end()
        block_end = starts[idx + 1].start() if idx + 1 < len(starts) else len(text)
        block = text[block_start:block_end]
        if not _KCONFIG_INT_TYPE_RE.search(block):
            continue
        base = None
        for val, cond in _KCONFIG_DEFAULT_RE.findall(block):
            if not cond.strip():
                base = int(val)
                break
        if base is not None:
            result[name] = base
    return result


def _collect_kconfig_int_defaults(ctx: Context) -> dict:
    """Glob every components/**/Kconfig + platform/**/Kconfig file and merge
    their int-typed base defaults into a single {name: default} map (first
    occurrence wins across files). Shared by kconfig-default-mismatch and
    kconfig-bridge-shadow so the glob+parse work isn't duplicated."""
    kconfig_defaults: dict[str, int] = {}
    for path in ctx.files(
        ["components/**/Kconfig", "platform/**/Kconfig"],
        exclude_dirs=[".pio", ".claude"],
    ):
        for name, base in _parse_kconfig_int_defaults(ctx.read(path)).items():
            kconfig_defaults.setdefault(name, base)
    return kconfig_defaults


def _check_kconfig_default_mismatch(ctx: Context) -> list:
    """Rule: kconfig-default-mismatch — for every `#ifndef BB_X` / `#define
    BB_X <int>` C fallback bridge, flag when it doesn't match the base
    (non-gated) default of the matching `config BB_X` Kconfig int entry.
    Enforces B1-459 (Kconfig/C default alignment) so it can't silently
    regress when either side is edited without the other."""
    violations = []
    root = Path(ctx.root)

    rule_cfg = ctx.config.get("lint", {}).get("rules", {}).get(
        "kconfig-default-mismatch", {}
    )
    allowlist: set = set(rule_cfg.get("allow", []))

    kconfig_defaults = _collect_kconfig_int_defaults(ctx)

    if not kconfig_defaults:
        return violations

    for path in ctx.files(
        ["components/**/*.c", "components/**/*.h",
         "platform/**/*.c", "platform/**/*.h", "platform/**/*.cpp"],
        exclude_dirs=[".pio", ".claude"],
    ):
        parts = path.relative_to(root).parts
        if "test" in parts:
            continue

        content = ctx.read(path)
        lines = content.splitlines()
        for i, line in enumerate(lines):
            m = _C_IFNDEF_BB_RE.match(line)
            if not m:
                continue
            name = m.group(1)
            if name in allowlist or name not in kconfig_defaults:
                continue
            if i + 1 >= len(lines):
                continue
            def_m = re.match(
                r'^\s*#define\s+' + re.escape(name) + r'\s+(-?\d+)\s*$',
                lines[i + 1],
            )
            if not def_m:
                continue
            c_default = int(def_m.group(1))
            k_default = kconfig_defaults[name]
            if c_default != k_default:
                key = f"{path.relative_to(root)}:{i + 2}"
                if key in allowlist:
                    continue
                violations.append(ctx.violation(
                    path, i + 2,
                    f"C fallback default {name}={c_default} != Kconfig base"
                    f" default {k_default}"))
    return violations


# ---------------------------------------------------------------------------
# Rule: task-creation-without-registration (19/19 bb_task_registry coverage)
# ---------------------------------------------------------------------------

_TASK_CREATE_RE = re.compile(
    r'\bxTaskCreate(?:StaticPinnedToCore|PinnedToCore|Static)?\s*\('
)
_TASK_REGISTRY_REGISTER_RE = re.compile(r'\bbb_task_registry_register\s*\(')


def _check_task_creation_without_registration(ctx: Context) -> list:
    """Rule: task-creation-without-registration — flags xTaskCreate/
    xTaskCreatePinnedToCore/*Static variants in components/ or
    platform/espidf/ that are not paired with a bb_task_registry_register(...)
    call anywhere in the same file. Enforces 19/19 task-registry coverage —
    every task created in breadboard's own tree must self-register so the
    software watchdog / task list can see it. SDK/vendor tasks live outside
    this tree and never trigger."""
    violations = []
    root = Path(ctx.root)

    rule_cfg = ctx.config.get("lint", {}).get("rules", {}).get(
        "task-creation-without-registration", {}
    )
    allowlist: set = set(rule_cfg.get("allow", []))

    for path in ctx.files(
        ["components/**/*.c", "components/**/*.h",
         "platform/espidf/**/*.c", "platform/espidf/**/*.h"],
        exclude_dirs=[".pio", ".claude"],
    ):
        parts = path.relative_to(root).parts
        if "test" in parts:
            continue

        rel = str(path.relative_to(root))
        if rel in allowlist:
            continue

        src = ctx.read(path)
        stripped = _strip_noise(src)

        create_matches = list(_TASK_CREATE_RE.finditer(stripped))
        if not create_matches:
            continue

        if _TASK_REGISTRY_REGISTER_RE.search(stripped):
            continue

        for m in create_matches:
            line_no = stripped[:m.start()].count('\n') + 1
            violations.append(ctx.violation(
                path, line_no,
                "task created without a paired bb_task_registry_register(...)"
                " in this file"))
    return violations


# ---------------------------------------------------------------------------
# Rule: public-requires-unused
# ---------------------------------------------------------------------------

# Seeded from the retired public-requires-watchlist rule's allowlist (that
# rule has been removed — this rule supersedes it) — these are known,
# deliberate exceptions to the REQUIRES/PRIV_REQUIRES
# decisive test (documented escape hatches, e.g. the large-panel LVGL
# backend exception in CLAUDE.md). Default, always active; a repo's
# bbtool.toml [lint.rules.public-requires-unused] allow = [[comp, dep], ...]
# adds to (not replaces) this list.
_PUBLIC_REQUIRES_UNUSED_DEFAULT_ALLOW = [
    ("bb_display_ek79007", "esp_lvgl_port"),
    ("bb_display_ek79007", "lv_"),
    ("bb_display_ek79007", "lvgl"),
    ("bb_display_ssd1306", "esp_driver_i2c"),
    ("bb_fan_emc2101", "esp_driver_i2c"),
    ("bb_power_tps546", "esp_driver_i2c"),
]

_INCLUDE_TARGET_RE = re.compile(r'#\s*include\s*[<"]([^">]+)[">]')
_REGISTER_CALL_RE = re.compile(r'\bidf_component_register\s*\(')


def _find_register_call_span(content: str) -> Optional[tuple]:
    """Return (open_paren_idx, close_paren_idx) for the first
    idf_component_register(...) call in content, or None if not found."""
    m = _REGISTER_CALL_RE.search(content)
    if not m:
        return None
    open_idx = content.index('(', m.start())
    close_idx = _walk_balanced(content, open_idx, '(', ')')
    if close_idx < 0:
        return None
    return open_idx, close_idx


# CMake component names sometimes don't match the header stem they provide
# (e.g. REQUIRES json provides cJSON.h; REQUIRES espressif__mdns provides
# mdns.h). Map the component-name token to the header-stem token it
# actually provides so these aren't false-flagged as unused.
_PUBLIC_REQUIRES_UNUSED_DEP_ALIASES = {
    "json": "cjson",
}


def _normalize_dep_token(dep: str) -> str:
    """Strip a leading 'espressif__' namespace and apply the known
    component-name -> header-stem alias map, for matching against
    #include'd header stems."""
    token = dep
    if token.startswith("espressif__"):
        token = token[len("espressif__"):]
    return _PUBLIC_REQUIRES_UNUSED_DEP_ALIASES.get(token, token)


def _dep_referenced_by_stem(dep: str, stem: str) -> bool:
    """True if a public header's #include stem satisfies REQUIRES dep,
    either literally or via the espressif__ / component-name alias map
    (case-insensitive for the alias path, since header stems like cJSON
    don't case-match their providing component name json)."""
    if stem == dep or stem.startswith(dep):
        return True
    normalized = _normalize_dep_token(dep)
    if normalized != dep:
        stem_lower = stem.lower()
        normalized_lower = normalized.lower()
        if stem_lower == normalized_lower or stem_lower.startswith(normalized_lower):
            return True
    return False


def _public_header_include_stems(comp_dir: Path, ctx: Context) -> set:
    """Return the set of filename stems (no extension) #include'd by any
    public header (include/*.h) of the component at comp_dir."""
    stems: set = set()
    inc_dir = comp_dir / "include"
    if not inc_dir.exists():
        return stems
    for header in sorted(inc_dir.glob("*.h")):
        content = ctx.read(header)
        for m in _INCLUDE_TARGET_RE.finditer(content):
            filename = m.group(1).rsplit('/', 1)[-1]
            stem = filename[:-2] if filename.endswith('.h') else filename
            stems.add(stem)
    return stems


def _check_public_requires_unused(ctx: Context) -> list:
    """Rule: public-requires-unused — for each component's REQUIRES (public)
    dependency, flag it when no public header (include/*.h) of the component
    #includes a header matching that dependency's name. This is the decisive
    test from CLAUDE.md's "REQUIRES vs PRIV_REQUIRES" section: a dep belongs
    in REQUIRES only if THIS component's public header includes that dep's
    header or uses its types — everything else defaults to PRIV_REQUIRES.

    Conservative on purpose (bias toward NOT flagging when unsure): a token T
    counts as "referenced" if any public header #includes a header whose
    filename stem equals T or starts with T. PRIV_REQUIRES tokens are never
    checked (they're already private).

    Scoped to platform/third-party tokens only (any dep NOT starting with
    "bb_" — esp_*, espressif__*, managed-component/bare names like lwip,
    mbedtls, mdns, cjson, etc.). Internal bb_*-to-bb_* coupling is out of
    scope: their types can reach a public header via transitive/cross-header
    includes the filename-stem heuristic can't follow, so evaluating them
    produced mostly false positives (~76 hits, collapsing to a handful of
    genuine platform leaks once bb_* tokens are excluded). This targets the
    esp_netif-class leak specifically.

    Severity stays "warn" — bb_wifi/esp_netif is a known, currently-unfixed
    leak; promoting to "error" here would break `make lint`/CI on the
    existing tree. Graduate to "error" once that leak is fixed downstream."""
    violations = []

    rule_cfg = ctx.config.get("lint", {}).get("rules", {}).get(
        "public-requires-unused", {}
    )
    config_allow = [tuple(pair) for pair in rule_cfg.get("allow", [])]
    allowlist = _PUBLIC_REQUIRES_UNUSED_DEFAULT_ALLOW + config_allow

    def is_allowlisted(comp: str, dep: str) -> bool:
        for (ac, ap) in allowlist:
            if comp == ac and dep.startswith(ap):
                return True
        return False

    for cmake_file in _component_cmake_files(ctx):
        comp = cmake_file.parent.name
        content = ctx.read(cmake_file)
        try:
            requires, _priv_requires = parse_requires(content, component=comp)
        except ConditionalSetError:
            # Out of scope for this rule — parse_requires already documents
            # why conditional REQUIRES/PRIV_REQUIRES isn't statically derivable.
            continue

        if not requires:
            continue

        stems = _public_header_include_stems(cmake_file.parent, ctx)

        # Line attribution: find the register call's own line as a
        # fallback, and pin per-dep violations to the line inside the call
        # block that word-boundary-matches the dep token (multi-line
        # REQUIRES lists are common — falling back to line 1 unconditionally
        # mis-attributes every one of them).
        register_span = _find_register_call_span(content)
        if register_span is not None:
            reg_open_idx, reg_close_idx = register_span
            register_line_no = content[:reg_open_idx].count('\n') + 1
            block_lines = content[reg_open_idx:reg_close_idx + 1].splitlines()
        else:
            register_line_no = 1
            block_lines = []

        for dep in requires:
            if dep.startswith("bb_"):
                # Internal bb_*-to-bb_* coupling — deliberately out of scope,
                # see the rule docstring.
                continue
            if is_allowlisted(comp, dep):
                continue
            if any(_dep_referenced_by_stem(dep, stem) for stem in stems):
                continue

            line_no = register_line_no
            dep_re = re.compile(r'\b' + re.escape(dep) + r'\b')
            for offset, line in enumerate(block_lines):
                if "PRIV_REQUIRES" in line:
                    continue
                if dep_re.search(line):
                    line_no = register_line_no + offset
                    break

            violations.append(ctx.violation(
                cmake_file, line_no,
                f"component={comp} dep={dep} — no public header references it;"
                " should be PRIV_REQUIRES"))

    return violations


# ---------------------------------------------------------------------------
# Rule: kconfig-bridge-shadow
# ---------------------------------------------------------------------------

def _check_kconfig_bridge_shadow(ctx: Context) -> list:
    """Rule: kconfig-bridge-shadow — flags a bare `#ifndef BB_X` / `#define
    BB_X <literal>` C fallback for a name X that also has a `config BB_X` int
    declared in Kconfig, when the same file has NO bridge (`#ifdef
    CONFIG_BB_X` / `#define BB_X CONFIG_BB_X`) tying the C macro to the
    Kconfig symbol. Without the bridge, the `#ifndef` always wins and the
    Kconfig knob can never reach the code — this shipped 3x (bb_net_health,
    bb_power_health, bb_pub; see CLAUDE.md 'Avoiding audit-class
    regressions')."""
    violations = []
    root = Path(ctx.root)

    kconfig_defaults = _collect_kconfig_int_defaults(ctx)

    if not kconfig_defaults:
        return violations

    for path in ctx.files(
        ["components/**/*.c", "components/**/*.h",
         "platform/**/*.c", "platform/**/*.h", "platform/**/*.cpp"],
        exclude_dirs=[".pio", ".claude"],
    ):
        parts = path.relative_to(root).parts
        if "test" in parts:
            continue

        content = ctx.read(path)
        lines = content.splitlines()

        for i, line in enumerate(lines):
            m = _C_IFNDEF_BB_RE.match(line)
            if not m:
                continue
            name = m.group(1)
            if name not in kconfig_defaults:
                continue
            if i + 1 >= len(lines):
                continue
            def_m = re.match(r'^\s*#define\s+' + re.escape(name) + r'\b', lines[i + 1])
            if not def_m:
                continue

            bridge_token = f"CONFIG_{name}"
            if re.search(rf'\bCONFIG_{re.escape(name)}\b', content):
                continue  # bridge exists somewhere in this file

            violations.append(ctx.violation(
                path, i + 1,
                f"{name} has a bare #ifndef/#define fallback but Kconfig declares"
                f" config {name} with no {bridge_token} bridge in this file —"
                " the knob is silently inert"))

    return violations


# ---------------------------------------------------------------------------
# Rule: raw-timestamp-divide
# ---------------------------------------------------------------------------

_RAW_TIMESTAMP_DIVIDE_RE = re.compile(
    r'\besp_timer_get_time\s*\(\s*\)\s*/\s*1000(?![0-9])[uUlL]*'
    r'|\bbb_timer_now_us\s*\(\s*\)\s*/\s*1000(?![0-9])[uUlL]*'
)


def _check_raw_timestamp_divide(ctx: Context) -> list:
    """Rule: raw-timestamp-divide — flags raw millisecond conversions
    (`esp_timer_get_time()/1000` or `bb_timer_now_us()/1000`) that bypass the
    canonical bb_clock helper, outside the bb_clock/ and bb_timer/ component
    directories (the only places allowed to do this raw math). See CLAUDE.md
    'Avoiding audit-class regressions' — timestamps must go through
    bb_clock_now_ms64() / bb_clock_now_ms()."""
    violations = []
    root = Path(ctx.root)

    rule_cfg = ctx.config.get("lint", {}).get("rules", {}).get(
        "raw-timestamp-divide", {}
    )
    allowlist: set = set(rule_cfg.get("allow", []))

    for path in ctx.files(
        ["platform/**/*.c", "platform/**/*.h",
         "components/**/*.c", "components/**/*.h"],
        exclude_dirs=[".pio", ".claude"],
    ):
        parts = path.relative_to(root).parts
        if "test" in parts:
            continue
        # Exempt the canonical bb_clock impl (basename bb_clock.c/.h, any
        # directory) and any file under a bb_timer component directory — the
        # only places allowed to do this raw math. Keying on a path
        # component literally named "bb_clock" misses the real layout
        # (platform/{host,espidf}/bb_core/bb_clock.c — component bb_core,
        # no bb_clock directory).
        if path.name in ("bb_clock.c", "bb_clock.h") or "bb_timer" in parts:
            continue

        rel = str(path.relative_to(root))
        if rel in allowlist:
            continue

        content = ctx.read(path)
        stripped = _strip_noise(content)
        for i, line in enumerate(stripped.splitlines(), 1):
            if not _RAW_TIMESTAMP_DIVIDE_RE.search(line):
                continue
            key = f"{rel}:{i}"
            if key in allowlist:
                continue
            violations.append(ctx.violation(path, i, line.strip()))

    return violations


# ---------------------------------------------------------------------------
# Rule: emit-seam-unwired-subscriber (B1-740)
# ---------------------------------------------------------------------------

# Pass A: BB_CALLBACK_SLOT_VOID(slot, bb_emit_fn, setter, invoke, ...) --
# matches on the bb_emit_fn callback TYPE specifically (not a *_set_emit
# name heuristic), so it's generic over any emit-seam instantiation, not
# just bb_wifi's.
_EMIT_SEAM_SLOT_RE = re.compile(
    r'\bBB_CALLBACK_SLOT_VOID\s*\(\s*\w+\s*,\s*bb_emit_fn\s*,\s*(\w+)\s*,\s*(\w+)\s*,'
)

_EVENT_SUBSCRIBE_RE = re.compile(r'\bbb_event_subscribe\s*\(')


def _emit_seam_owner_from_path(root: Path, path: Path) -> Optional[str]:
    """Derive the owning component name from a source path via the
    discovery index (B1-979) rather than a hand-rolled path-position
    encoding: components/<name>/... -> name; platform/<plat>/<name>/... ->
    name."""
    return build_index([str(root)]).owner_of_path(path)


def _emit_seam_invoke_topic(content: str, invoke: str) -> Optional[str]:
    """Pass B: find the topic token passed as the invoke fn's first arg in
    the same file (string literal or ALL_CAPS macro).

    Assumption: one topic per seam per file -- `re.search` returns only the
    FIRST invoke call-site's topic. A seam whose invoke fires with different
    topics from multiple call sites in the same file would be under-checked
    (no current seam does this; not handled)."""
    m = re.search(
        re.escape(invoke) + r'\s*\(\s*([A-Z_][A-Z0-9_]*|"[^"]*")', content
    )
    return m.group(1) if m else None


def _discover_emit_seams(ctx: Context) -> list:
    """Pass A+B, repo-wide: every emit-seam publisher (BB_CALLBACK_SLOT_VOID
    over a bb_emit_fn slot) plus the topic token its invoke fn publishes.
    Returns [{owner, setter, invoke, topic, path}, ...]."""
    root = Path(ctx.root)
    seams = []
    for path in ctx.files(
        ["components/**/*.c", "platform/**/*.c"],
        exclude_dirs=[".pio", ".claude"],
    ):
        parts = path.relative_to(root).parts
        if "test" in parts:
            continue
        content = ctx.read(path)
        for m in _EMIT_SEAM_SLOT_RE.finditer(content):
            setter, invoke = m.group(1), m.group(2)
            owner = _emit_seam_owner_from_path(root, path)
            if owner is None:
                continue
            topic = _emit_seam_invoke_topic(content, invoke)
            if topic is None:
                continue
            seams.append({
                "owner": owner, "setter": setter, "invoke": invoke,
                "topic": topic, "path": path,
            })
    return seams


def _emit_seam_topic_re(topic: str) -> re.Pattern:
    """Compile a co-occurrence pattern for one topic token. A bare ALL_CAPS
    macro token is \\b-anchored (a raw substring check would false-match a
    longer identifier containing it, e.g. FOO_TOPIC inside FOO_TOPIC_EXTRA);
    a quoted string-literal token is already self-anchored by its quotes, so
    it's matched as a literal substring."""
    if topic.startswith('"'):
        return re.compile(re.escape(topic))
    return re.compile(r'\b' + re.escape(topic) + r'\b')


def _discover_emit_seam_subscribers(ctx: Context, topics: set) -> dict:
    """Pass C: same-file co-occurrence -- a file containing BOTH a target
    topic token AND a bb_event_subscribe( call is a subscriber of that
    topic (not a literal-arg match: real subscribers resolve the topic via
    a local var, e.g. bb_mdns.c/bb_mqtt_client_espidf.c). Returns
    {topic: {owner, ...}}."""
    root = Path(ctx.root)
    result: dict = {t: set() for t in topics}
    topic_res = {t: _emit_seam_topic_re(t) for t in topics}
    for path in ctx.files(
        ["components/**/*.c", "platform/**/*.c"],
        exclude_dirs=[".pio", ".claude"],
    ):
        parts = path.relative_to(root).parts
        if "test" in parts:
            continue
        content = ctx.read(path)
        if not _EVENT_SUBSCRIBE_RE.search(content):
            continue
        owner = _emit_seam_owner_from_path(root, path)
        if owner is None:
            continue
        for topic, topic_re in topic_res.items():
            if topic_re.search(content):
                result[topic].add(owner)
    return result


def _check_emit_seam_unwired_subscriber(ctx: Context) -> list:
    """Rule: emit-seam-unwired-subscriber -- for every emit-seam publisher
    (a `BB_CALLBACK_SLOT_VOID(slot, bb_emit_fn, setter, invoke, ...)`
    instantiation) whose published topic has a same-file co-occurring
    `bb_event_subscribe(` consumer, flag any app composition root
    (`examples/*/main/CMakeLists.txt`) whose transitive closure includes
    BOTH the seam's owning component and the subscriber's owning component,
    but never calls the seam's setter anywhere under that app's main/ tree
    -- i.e. the app links a publisher and a subscriber of the same topic
    but never actually wires them together (codegen and handwire are the
    only sanctioned composition paths; this seam wiring is neither, it's a
    third, easy-to-forget manual step).

    Generic over the emit-seam pattern (any bb_emit_fn slot) -- wifi.net/
    bb_wifi is just the first live instance (B1-740, under EPIC B1-742)."""
    violations = []
    root = Path(ctx.root)

    rule_cfg = ctx.config.get("lint", {}).get("rules", {}).get(
        "emit-seam-unwired-subscriber", {}
    )
    allowlist: set = set(rule_cfg.get("allow", []))

    seams = _discover_emit_seams(ctx)
    if not seams:
        return violations

    topics = {s["topic"] for s in seams}
    subscribers_by_topic = _discover_emit_seam_subscribers(ctx, topics)

    universe = discover_components(root)

    for cmake_path in sorted(root.glob("examples/*/main/CMakeLists.txt")):
        app_dir = cmake_path.parent  # examples/<name>/main
        app_name = cmake_path.parts[-3]
        rel_app = str(app_dir.relative_to(root))
        if rel_app in allowlist:
            continue

        content = ctx.read(cmake_path)
        try:
            requires, priv_requires = parse_requires(content, component=app_name)
        except ConditionalSetError as e:
            print(
                f"bbtool lint emit-seam-unwired-subscriber: skipping "
                f"{rel_app}: {e}",
                file=sys.stderr,
            )
            continue

        names = sorted({n for n in (requires + priv_requires) if n in universe})
        if not names:
            continue

        try:
            closure = set(resolve_composition(str(root), names, platform="espidf"))
        except ManifestError:
            continue
        except ConditionalSetError as e:
            # A transitively-visited DEPENDENCY component's own CMakeLists.txt
            # (not the app's, already guarded above) has a conditionally-set()
            # REQUIRES/PRIV_REQUIRES var (boards.derive_component ->
            # cmake_parse.parse_requires) -- skip this app the same way,
            # rather than letting it propagate uncaught out of the rule and
            # abort the whole lint run (lint.py's rule runner has no
            # try/except around rule.check(ctx)).
            print(
                f"bbtool lint emit-seam-unwired-subscriber: skipping "
                f"{rel_app}: {e}",
                file=sys.stderr,
            )
            continue

        app_src_parts = []
        for suffix in ("*.c", "*.cpp", "*.h"):
            for src_path in app_dir.rglob(suffix):
                if src_path.is_file():
                    app_src_parts.append(_strip_noise(ctx.read(src_path)))
        app_src = "\n".join(app_src_parts)

        for seam in seams:
            if seam["owner"] not in closure:
                continue
            wired_subscribers = subscribers_by_topic.get(seam["topic"], set()) & closure
            if not wired_subscribers:
                continue
            setter_re = re.compile(r'\b' + re.escape(seam["setter"]) + r'\s*\(')
            if setter_re.search(app_src):
                continue
            key = f"{rel_app}:{seam['setter']}"
            if key in allowlist:
                continue
            subs_str = ", ".join(sorted(wired_subscribers))
            violations.append(ctx.violation(
                cmake_path, 1,
                f"{seam['owner']} emit-seam (topic {seam['topic']}) has "
                f"subscriber(s) [{subs_str}] in {app_name}'s composition, but "
                f"{seam['setter']}(...) is never called under {rel_app} -- "
                "add a handwire, or a `// bbtool:init tier=... "
                "consumes=<key>` marker paired with a provider's `// "
                "bbtool:provides key=<key> symbol=<sym>` marker, "
                f"or allowlist {rel_app} if intentional",
            ))

    return violations


def _find_token_line(content: str, token: str) -> int:
    """Best-effort line number (1-based) of the first line in `content`
    containing `token` verbatim; falls back to line 1 when not found (e.g. a
    token synthesized via ${VAR} indirection that never appears literally in
    this file's own text)."""
    for i, line in enumerate(content.splitlines(), 1):
        if token in line:
            return i
    return 1


def _embed_assets_out_var(raw_token: str, embed_assets: dict) -> Optional[str]:
    """Return the `bb_embed_assets(OUT_SRCS <var> ...)` var name if
    `raw_token` is an exact `${<var>}` reference to a var `parse_embed_assets`
    modeled for this file, else `None`. A SRCS token matching this is
    generated at CMake configure time (not a literal on-disk path to check
    itself) — its ASSETS inputs are validated separately."""
    if raw_token.startswith("${") and raw_token.endswith("}"):
        var_name = raw_token[2:-1]
        if var_name in embed_assets:
            return var_name
    return None


def _opaque_var_exemption(raw_token: str, stripped_content: str,
                           single_opaque_vars: dict) -> bool:
    """True if `raw_token` is exempted from the on-disk existence check
    because it references (as a `${VAR}` substring, e.g. `${var}/suffix`,
    not just a bare-token match) a var in `single_opaque_vars`
    (`cmake_parse.single_opaque_property_vars` — a var whose ENTIRE
    assignment history in this file is exactly one
    `idf_component_get_property`/`idf_build_get_property` call) AND that
    property-get call precedes `raw_token`'s own occurrence in
    `stripped_content` (both measured in the SAME comment-stripped
    coordinate space `single_opaque_property_vars` uses).

    Provenance- and order-aware by construction (B1-1134 review HIGH fix):
    a var name reused for an unrelated `set()`/`list(APPEND)`/second
    property-get elsewhere in the file is never in `single_opaque_vars` at
    all (excluded upstream), and a reference that textually PRECEDES its
    property-get call is never exempted even if the var otherwise
    qualifies — a name-only, position-blind match (the original,
    vulnerable form of this check) would have silently exempted both.

    `raw_token`'s own position is approximated as the LEFTMOST occurrence
    of that exact token text in `stripped_content` (`str.find`) — the most
    CONSERVATIVE choice when the same literal token string appears more
    than once in a file (rare in practice): if even the earliest such
    occurrence doesn't textually follow the property-get, this correctly
    refuses to exempt, at worst producing an extra, loud, fixable
    violation rather than ever risking a false-negative exemption. If the
    token can't be located at all (`str.find` returns -1 — shouldn't
    happen, since every `raw_token` this is called with was extracted
    from `stripped_content` itself), this fails closed: never exempt
    without confirming ordering."""
    for var_name, prop_pos in single_opaque_vars.items():
        if ("${" + var_name + "}") not in raw_token:
            continue
        ref_pos = stripped_content.find(raw_token)
        if ref_pos != -1 and ref_pos > prop_pos:
            return True
    return False


# "file" -> must resolve to an existing file (SRCS, include()'s argument).
# "dir" -> must resolve to an existing directory (SRC_DIRS, INCLUDE_DIRS,
# PRIV_INCLUDE_DIRS, target_include_directories()'s arguments).
_PATH_KEYWORD_KIND = {
    "SRCS": "file",
    "SRC_DIRS": "dir",
    "INCLUDE_DIRS": "dir",
    "PRIV_INCLUDE_DIRS": "dir",
}


def _resolve_cmake_path_token(comp_dir: Path, label: str, raw_token: str,
                               kind: str) -> Optional[str]:
    """Resolve one path-bearing CMake token to an on-disk path and check it
    exists (`kind="file"` or `kind="dir"`). Returns `None` if it resolves
    and exists, else a detail string describing the failure — NEVER
    silently passes a token it couldn't fully resolve; an unrecognized
    `${VAR}` still left in the token after `${CMAKE_CURRENT_LIST_DIR}`
    substitution is itself a failure, not a skip. The opaque-var exemption
    (`_opaque_var_exemption`) is checked by the CALLER, before this
    function runs — it needs the whole file's comment-stripped text and
    var-assignment-position map, which this single-token function
    deliberately doesn't carry."""
    substituted = raw_token.replace("${CMAKE_CURRENT_LIST_DIR}", str(comp_dir))
    if "${" in substituted:
        return f"{label} token '{raw_token}' — unresolved CMake variable"

    p = Path(substituted)
    if not p.is_absolute():
        p = comp_dir / substituted

    exists = p.is_file() if kind == "file" else p.is_dir()
    if exists:
        return None
    kind_name = "file" if kind == "file" else "directory"
    return f"{label} token '{raw_token}' -> {p} — {kind_name} does not exist"


# label -> (parser_fn, kind) for every path-bearing CMake command this rule
# covers in a component's own CMakeLists.txt, beyond idf_component_register
# itself (handled separately via parse_paths, since it has 4 keyword-scoped
# sub-lists rather than one flat list). Each parser_fn(content) -> [token,
# ...]. Adding a newly-covered command is a matter of adding an entry here
# (data, not a new code path) -- see _check_component_path_unresolved's
# docstring for the full command inventory, including deliberately
# excluded commands and why.
_EXTRA_PATH_COMMANDS = (
    ("target_include_directories", parse_target_include_directories, "dir"),
    ("include", parse_include_calls, "file"),
)


def _check_component_path_unresolved(ctx: Context) -> list:
    """Rule: component-path-unresolved — every filesystem path token a
    component's own CMakeLists.txt or an example's platformio.ini references
    must actually resolve on disk.

    Components move between flat and grouped `components/<group>/<name>/`
    layouts (B1-1134); moving one silently breaks any
    `${CMAKE_CURRENT_LIST_DIR}/../../...` path that assumed the OLD nesting
    depth — nothing catches that until a build fails, far from the
    CMakeLists.txt that actually moved. This includes `include(...)`'s own
    argument (`components/bb_prov_default_form/CMakeLists.txt:1` includes
    `cmake/bbtool.cmake` via a depth-dependent relative path — it breaks the
    same way a stale SRCS path does).

    `examples/*/platformio.ini` gets the same treatment for a second,
    sneakier failure mode: PlatformIO's `build_src_filter` `+<...>` glob
    entries do NOT error when a pattern matches zero files — the source
    silently drops out of the build and only surfaces later as an
    undefined-reference at LINK time, arbitrarily far from the actual stale
    path. `-I<dir>` `build_flags` entries get the same existence check.

    ## Command inventory (components/**/CMakeLists.txt)

    Covered, via `cmake_parse.parse_paths` (SRCS/SRC_DIRS/INCLUDE_DIRS/
    PRIV_INCLUDE_DIRS inside `idf_component_register(...)`),
    `parse_target_include_directories` (`target_include_directories(...)`'s
    directory arguments), and `parse_include_calls` (`include(...)`'s
    file argument, when path-shaped — see that function's docstring for
    the bare-CMake-module-name carve-out, which is a different resolution
    mechanism, not a violation or a guess). `_EXTRA_PATH_COMMANDS` is the
    single place a future command's parser gets registered — the block-
    finder underneath (`cmake_parse._iter_call_blocks`) already takes a SET
    of command names, so adding one is data, not a new code path.

    Deliberately excluded, documented here per B1-1134 review (an implicit
    gap is exactly what produced that finding):

    - `target_link_libraries(...)` — every use in this tree passes a
      linker FLAG (`"-u <symbol>"`, force-keeping a constructor across
      `--gc-sections`), never a filesystem path. A real library-path
      argument would need CMake target-name/generator-expression
      resolution (`$<TARGET_FILE:...>` etc.), which is out of scope for a
      text-only parser; if one ever appears, it needs a deliberate,
      reviewed extension here, not a silent pass.
    - `idf_build_set_property(...)` — a property SETTER, not a path-bearing
      call; the one use in this tree (`bb_http_server`) sets
      `COMPILE_OPTIONS` to a compiler flag string.
    - `idf_component_get_property`/`idf_build_get_property` — these ARE
      covered, but NOT as commands with their own path argument to check:
      they assign a var from ESP-IDF's own build-graph state (e.g.
      `espcoredump`'s `COMPONENT_DIR`) at CMake configure time, which this
      text-only parser cannot simulate. `cmake_parse.single_opaque_property_vars`
      collects every var whose ENTIRE assignment history in the file is
      EXACTLY ONE such call; any path token ELSEWHERE (e.g. `bb_diag`'s
      `target_include_directories(... ${espcoredump_dir}/include)`) that
      references one of those vars, AND whose own occurrence textually
      FOLLOWS that property-get call, is a MODELED, documented exclusion
      from the existence check (`_opaque_var_exemption`) — not a violation
      (not claiming it's broken) and not a guess (never fabricates the
      var's value). Deliberately provenance- AND order-aware (B1-1134
      review HIGH): a var name reused for an unrelated `set()`/
      `list(APPEND)`/second property-get elsewhere in the file, or
      referenced BEFORE its property-get call, is NEVER exempted, even if
      SOME occurrence of that name is legitimately a property lookup — a
      flat "was this name ever assigned via a property call anywhere in
      the file" check would silently exempt a fabricated, unrelated path
      that merely happens to reuse a short var name like `dir`.
    - `target_sources`, `add_subdirectory`, `set_source_files_properties`,
      `file(...)` (GLOB/COPY/etc.) — NOT present anywhere in
      `components/**/CMakeLists.txt` today (confirmed by inventory sweep,
      B1-1134 review), so there is nothing to cover yet. If one of these
      is ever introduced, extend `_EXTRA_PATH_COMMANDS` (and
      `cmake_parse.py` with a matching `parse_*` function) the same way
      `target_include_directories`/`include` were added here — never add a
      component using one of these without also covering it.

    Reuses `cmake_parse`'s parsers (never a second hand-rolled CMake path
    parser) and `discovery.build_index` via `_component_cmake_files` (never
    a hand-rolled `iterdir()` walk — discovery's leaf rule is depth-agnostic,
    so this rule doesn't care whether a component is flat or grouped).

    Fails loud, never skips: every token is either resolved, MODELED (a
    known-semantics construct — see above and below), or a violation.
    There is no fourth category, and never a silent pass:

    - A path keyword `${VAR}` fed by a genuinely conditional `set()` inside
      an `if()/elseif()/else()` block does NOT need to guess which branch
      is build-time-live (unlike `parse_requires`'s REQUIRES/PRIV_REQUIRES,
      where the branches mean different, mutually-exclusive things) —
      every path-bearing parser above branch-enumerates instead (via
      `cmake_parse._expand_path_token`), and EVERY branch's path gets
      checked. This is strictly more coverage than a skip, never less: a
      component whose relative-path depth breaks after a group-move is
      still caught even if the broken branch isn't the "obvious" one.
    - A `${VAR}` populated by `bb_embed_assets(OUT_SRCS <var> ASSETS
      <file>:<symbol> ...)` (`cmake/bbtool.cmake`'s configure-time asset
      embed macro) is MODELED via `cmake_parse.parse_embed_assets`: the
      generated `.c` byte-array source genuinely doesn't exist on disk
      until CMake runs, so the OUT_SRCS token itself is exempt from the
      on-disk SRCS check — but every one of its `ASSETS` INPUT files is
      validated instead (must exist, relative to the component's own
      directory, mirroring `bb_embed_assets`'s own documented resolution
      contract). Net effect: more coverage than before (a renamed/deleted
      asset input is now caught), never an exemption.

    ## C/C++ `#include "..."` relative paths (B1-1140)

    The two checks above catch a stale CMakeLists.txt/platformio.ini path,
    but a source file's own `#include "../../components/..."` relative
    path was invisible to this rule — five such stale includes had to be
    found by exhaustive grep during the B1-980 family-group move (test/
    fixtures and platform/ backends referencing a component's internal
    header across the move). Scans EVERY `.c`/`.cpp`/`.h` file in the tree
    (not just `components/**`, since the whole point is that `examples/`,
    `platform/`, and `test/` sources reach INTO `components/`), resolves
    each quoted `#include` token relative to the INCLUDING FILE's own
    directory (the real preprocessor's resolution rule — distinct from the
    CMakeLists.txt checks above, which resolve relative to the component's
    own directory), and only checks tokens that LITERALLY contain the
    substring `components/` (the B1-1140 motivating shape: a relative
    `../../components/<name>/...` navigation that escapes the including
    file's own tree to reach another component). This deliberately
    excludes two other quoted-include shapes that also contain `/` but are
    search-path-resolved, not relative-path-resolved, and this rule can't
    simulate without the full component dependency graph (out of scope for
    a per-file text scan): (1) a dependency's header referenced by bare
    filename or its own internal subpath (`#include "bb_core.h"`),
    resolved via the component's `INCLUDE_DIRS`/`REQUIRES` search path;
    (2) an ESP-IDF SDK header written with quotes (ESP-IDF's own
    convention), e.g. `#include "freertos/FreeRTOS.h"` or `"driver/
    i2c_master.h"` — its naive same-directory join happens to land under
    `components/` purely because the including file already lives there,
    not because the token itself navigates there. Fails loud like every
    other check here: a token that DOES literally spell `components/` and
    doesn't resolve to an existing file is a violation, never a skip.

    Residual gap: this check is scoped to tokens that literally spell
    `components/`, so it does NOT cover the same failure shape (a relative
    include silently breaking on a directory move) when the navigation
    stays within `platform/` or crosses from `test/` into `platform/` —
    e.g. `test/test_host/test_main.c`'s `../../platform/host/bb_wdt/
    bb_wdt_test.h`, or `platform/espidf/bb_log_event/bb_log_event.c`'s
    `../../host/bb_log_event/bb_log_event_parse.h`. Extending coverage to
    that class is deliberately deferred to its own change (it would
    surface pre-existing paths needing their own separate fixes) — see
    B1-1143."""
    violations = []
    root = Path(ctx.root)

    # -- 1. Component CMakeLists.txt.
    for cmake_file in _component_cmake_files(ctx):
        comp_dir = cmake_file.parent
        comp_name = comp_dir.name
        content = ctx.read(cmake_file)
        stripped_content = strip_cmake_comments(content)
        embed_assets = parse_embed_assets(content)
        single_opaque_vars = single_opaque_property_vars(content)
        paths = parse_paths(content, component=comp_name)

        # SRCS / SRC_DIRS / INCLUDE_DIRS / PRIV_INCLUDE_DIRS (idf_component_
        # register(...)). A SRCS token that's exactly
        # `${<bb_embed_assets OUT_SRCS var>}` is modeled (its ASSETS inputs
        # are validated separately below) rather than checked as a literal
        # on-disk path.
        for keyword, tokens in paths.items():
            kind = _PATH_KEYWORD_KIND[keyword]
            for raw_token in tokens:
                embed_var = _embed_assets_out_var(raw_token, embed_assets)
                if embed_var is not None:
                    continue
                if _opaque_var_exemption(raw_token, stripped_content, single_opaque_vars):
                    continue
                detail = _resolve_cmake_path_token(comp_dir, keyword, raw_token, kind)
                if detail is None:
                    continue
                line_no = _find_token_line(content, raw_token)
                violations.append(ctx.violation(
                    cmake_file, line_no, f"component={comp_name} {detail}"))

        # Every other covered path-bearing command (target_include_directories,
        # include(...), ... -- see _EXTRA_PATH_COMMANDS).
        for label, parser_fn, kind in _EXTRA_PATH_COMMANDS:
            for raw_token in parser_fn(content):
                if _opaque_var_exemption(raw_token, stripped_content, single_opaque_vars):
                    continue
                detail = _resolve_cmake_path_token(comp_dir, label, raw_token, kind)
                if detail is None:
                    continue
                line_no = _find_token_line(content, raw_token)
                violations.append(ctx.violation(
                    cmake_file, line_no, f"component={comp_name} {detail}"))

        # bb_embed_assets(...)'s ASSETS input files.
        for out_var, asset_files in embed_assets.items():
            for asset_file in asset_files:
                asset_path = Path(asset_file)
                if not asset_path.is_absolute():
                    asset_path = comp_dir / asset_file
                if asset_path.is_file():
                    continue
                line_no = _find_token_line(content, asset_file)
                violations.append(ctx.violation(
                    cmake_file, line_no,
                    f"component={comp_name} bb_embed_assets(OUT_SRCS "
                    f"{out_var} ...) ASSETS token '{asset_file}' -> "
                    f"{asset_path} — file does not exist"))

    # -- 2. examples/*/platformio.ini: -I<dir> build_flags entries (must
    #    resolve to an existing directory, relative to the ini file's own
    #    directory -- PlatformIO resolves compiler -I flags relative to the
    #    project dir) and +<...> build_src_filter entries (must match at
    #    least one existing file, relative to PlatformIO's own `src_dir`
    #    setting -- PlatformIO resolves src_filter patterns relative to
    #    src_dir, not the ini's own directory; default src_dir is "src" when
    #    the [platformio] section omits it).
    for ini_path in sorted(root.glob("examples/*/platformio.ini")):
        ini_dir = ini_path.parent
        parser = configparser.ConfigParser(interpolation=None)
        try:
            parser.read(ini_path, encoding="utf-8")
        except configparser.Error as e:
            violations.append(ctx.violation(
                ini_path, 1, f"unparseable platformio.ini: {e}"))
            continue

        src_dir = "src"
        if parser.has_option("platformio", "src_dir"):
            src_dir = parser.get("platformio", "src_dir")
        src_base = ini_dir / src_dir

        for section in parser.sections():
            if parser.has_option(section, "build_flags"):
                value = parser.get(section, "build_flags")
                line_no = _find_token_line(ctx.read(ini_path), "build_flags")
                for tok in value.split():
                    if not tok.startswith("-I"):
                        continue
                    inc = tok[2:]
                    inc_path = Path(inc)
                    if not inc_path.is_absolute():
                        inc_path = ini_dir / inc
                    if not inc_path.is_dir():
                        violations.append(ctx.violation(
                            ini_path, line_no,
                            f"[{section}] build_flags -I{inc} -> "
                            f"{inc_path} — directory does not exist"))

            if parser.has_option(section, "build_src_filter"):
                value = parser.get(section, "build_src_filter")
                line_no = _find_token_line(ctx.read(ini_path), "build_src_filter")
                for tok in value.split():
                    if not tok.startswith("+<"):
                        continue
                    if not tok.endswith(">"):
                        violations.append(ctx.violation(
                            ini_path, line_no,
                            f"[{section}] build_src_filter malformed entry "
                            f"'{tok}' — missing closing '>'"))
                        continue
                    pattern = tok[2:-1]
                    if not pattern:
                        violations.append(ctx.violation(
                            ini_path, line_no,
                            f"[{section}] build_src_filter empty +<> entry"))
                        continue
                    matches = list(src_base.glob(pattern))
                    if not any(m.is_file() for m in matches):
                        violations.append(ctx.violation(
                            ini_path, line_no,
                            f"[{section}] build_src_filter +<{pattern}> "
                            f"(base {src_base}) — matches zero files"))

    # -- 3. C/C++ `#include "..."` relative paths reaching into components/
    #    (B1-1140) -- see the "C/C++ #include" section of this function's
    #    docstring. Tree-wide scan: the whole point is catching a stale
    #    include in examples/, platform/, or test/, not just components/
    #    itself.
    components_root = root / "components"
    include_re = re.compile(r'^\s*#\s*include\s*"([^"]+)"')
    for src_path in ctx.files(
        ["**/*.c", "**/*.cpp", "**/*.h"],
        exclude_dirs=[".pio", ".claude"],
    ):
        content = ctx.read(src_path)
        for i, line in enumerate(content.splitlines(), 1):
            m = include_re.match(line)
            if not m:
                continue
            token = m.group(1)
            if "components/" not in token:
                # Two distinct search-path-resolved shapes this rule must
                # NOT guess at, both of which have '/' in the token so a
                # bare-filename check alone wouldn't exclude them: (1) a
                # dependency's own header by bare filename or its own
                # internal subpath (e.g. #include "bb_core.h") -- resolved
                # via the component's INCLUDE_DIRS/REQUIRES search path,
                # not relative to the including file's directory; (2) an
                # ESP-IDF SDK header written with quotes (ESP-IDF's own
                # convention) whose first segment names an SDK component,
                # e.g. #include "freertos/FreeRTOS.h" or "driver/
                # i2c_master.h" -- also search-path-resolved, and its
                # naive same-directory join happens to land under
                # components/ purely because the including file already
                # lives there, not because the token itself navigates
                # there. This rule only models a token that LITERALLY
                # spells out a path reaching into components/ (the B1-1140
                # motivating shape: "../../components/<name>/...") --
                # simulating the full component dependency graph is out of
                # scope for a per-file text scan.
                continue
            resolved = Path(os.path.normpath(str(src_path.parent / token)))
            try:
                resolved.relative_to(components_root)
            except ValueError:
                # Doesn't reach into components/ -- out of this rule's scope.
                continue
            if resolved.is_file():
                continue
            violations.append(ctx.violation(
                src_path, i,
                f'#include "{token}" -> {resolved} — file does not exist'))

    return violations


# ---------------------------------------------------------------------------
# Rule registry
# ---------------------------------------------------------------------------

_LINT_RULES: dict[str, Rule] = {}


def _register_lint_rules() -> None:
    rules = [
        Rule(
            id="deprecated-http-send",
            default_severity="error",
            profiles={"all"},
            check=_check_deprecated_http_send,
            hint="use bb_http_resp_send_chunk / bb_http_resp_sendstr",
        ),
        Rule(
            id="public-header-leak",
            default_severity="error",
            profiles={"library"},
            check=_check_public_header_leak,
            hint="gate esp_ includes with #ifdef ESP_PLATFORM",
        ),
        Rule(
            id="state-topic-post",
            default_severity="error",
            profiles={"all"},
            check=_check_state_topic_post,
            hint="state topics must be posted through bb_cache",
        ),
        Rule(
            id="raw-allocator",
            default_severity="error",
            profiles={"all"},
            check=_check_raw_allocator,
            hint="use bb_malloc_prefer_spiram/bb_calloc_prefer_spiram/bb_mem_free"
                 " instead of raw malloc/calloc/free",
        ),
        Rule(
            id="raw-esp-timer",
            default_severity="error",
            profiles={"all"},
            check=_check_raw_esp_timer,
            hint="use bb_timer_deferred_* / bb_timer_worker_* (never raw esp_timer_create)",
        ),
        Rule(
            id="timer-cb-heavy",
            default_severity="error",
            profiles={"all"},
            check=_check_timer_cb_heavy,
            hint="timer callback does heavy work — use bb_timer_deferred_*",
        ),
        Rule(
            id="platform-error-in-public-struct",
            default_severity="warn",
            profiles={"library"},
            check=_check_platform_error_in_public_struct,
            hint="public structs must not surface raw platform error codes as scalars"
                 " — use a portable bb_* enum or keep diagnostic/log-only",
        ),
        Rule(
            id="ticket-ref-in-log",
            default_severity="error",
            profiles={"all"},
            check=_check_ticket_ref_in_log,
            hint="no ticket IDs in runtime log strings — reference tickets in comments only",
        ),
        Rule(
            id="bb-prefix",
            default_severity="warn",
            profiles={"library"},
            check=_check_bb_prefix,
            hint="public symbols must be bb_-prefixed (v0.1.0 convention)",
        ),
        Rule(
            id="pragma-once",
            default_severity="error",
            profiles={"library"},
            check=_check_pragma_once,
            hint="use #pragma once (not #ifndef include guards)",
        ),
        Rule(
            id="no-arduino-string",
            default_severity="error",
            profiles={"library"},
            check=_check_no_arduino_string,
            hint="no Arduino String in library code — use const char* + length",
        ),
        Rule(
            id="public-header-inline-platform-call",
            default_severity="error",
            profiles={"library"},
            check=_check_public_header_inline_platform_call,
            hint="public-header inline functions must not call platform APIs (esp_*, FreeRTOS, lwip)"
                 " — they leak the dep into every consumer's TU; de-inline into the platform impl",
        ),
        Rule(
            id="mutating-route-needs-body-schema",
            default_severity="error",
            profiles={"all"},
            check=_check_mutating_route_needs_body_schema,
            hint="POST/PATCH/PUT routes with a JSON/form body must have a non-bare"
                 " .request_schema with properties — prevents silent OpenAPI contract gaps",
        ),
        Rule(
            id="event-topic-needs-schema",
            default_severity="error",
            profiles={"all"},
            check=_check_event_topic_needs_schema,
            hint="every SSE topic attached via bb_event_routes_attach* must have a"
                 " schema registered via bb_openapi_register_topic_schema or"
                 " bb_openapi_register_schema(sse_topic!=NULL)",
        ),
        Rule(
            id="kconfig-default-mismatch",
            default_severity="error",
            profiles={"all"},
            check=_check_kconfig_default_mismatch,
            hint="C `#ifndef BB_X #define BB_X <val>` fallback default must match"
                 " the base (non-gated) `config BB_X` Kconfig default (B1-459)",
        ),
        Rule(
            id="task-creation-without-registration",
            default_severity="error",
            profiles={"all"},
            check=_check_task_creation_without_registration,
            hint="every xTaskCreate*/xTaskCreateStatic* in components/ or"
                 " platform/espidf/ must pair with bb_task_registry_register(...)"
                 " in the same file",
        ),
        Rule(
            id="public-requires-unused",
            default_severity="warn",
            profiles={"library"},
            check=_check_public_requires_unused,
            hint="REQUIRES dep not referenced by any public header — move to"
                 " PRIV_REQUIRES (decisive test: CLAUDE.md 'REQUIRES vs PRIV_REQUIRES')",
        ),
        Rule(
            id="kconfig-bridge-shadow",
            default_severity="error",
            profiles={"all"},
            check=_check_kconfig_bridge_shadow,
            hint="bare #ifndef BB_X/#define BB_X shadows a Kconfig config BB_X"
                 " with no CONFIG_BB_X bridge — the knob is silently inert",
        ),
        Rule(
            id="raw-timestamp-divide",
            default_severity="warn",
            profiles={"all"},
            check=_check_raw_timestamp_divide,
            hint="use bb_clock_now_ms64()/bb_clock_now_ms() instead of raw"
                 " esp_timer_get_time()/1000 or bb_timer_now_us()/1000"
                 " outside bb_clock/ and bb_timer/",
        ),
        Rule(
            id="emit-seam-unwired-subscriber",
            default_severity="error",
            profiles={"all"},
            check=_check_emit_seam_unwired_subscriber,
            hint="an app links a bb_emit_fn seam publisher and a subscriber of"
                 " its topic but never calls the seam's setter -- add a"
                 " handwire, a `// bbtool:init tier=... consumes=<key>`"
                 " marker paired with a provider's `// bbtool:provides"
                 " key=<key> symbol=<sym>` marker, or allowlist the app",
        ),
        Rule(
            id="component-path-unresolved",
            default_severity="error",
            profiles={"library"},
            check=_check_component_path_unresolved,
            hint="a component CMakeLists.txt, example platformio.ini, or a"
                 " source file's own #include \"...\" references a"
                 " filesystem path into components/ that doesn't resolve"
                 " on disk -- PlatformIO's build_src_filter +<...> silently"
                 " drops zero-match entries instead of erroring, surfacing"
                 " only as a link-time undefined reference, and a stale"
                 " relative #include fails loud only at compile time, far"
                 " from the component move that broke it",
        ),
    ]
    for rule in rules:
        _LINT_RULES[rule.id] = rule


_register_lint_rules()


# ---------------------------------------------------------------------------
# Plugin bus registration
# ---------------------------------------------------------------------------

def register(api) -> None:
    import sys
    mod = sys.modules[__name__]
    api.add_command(NAME, mod)
    for rule in _LINT_RULES.values():
        api.add_rule(rule)


# ---------------------------------------------------------------------------
# Command interface
# ---------------------------------------------------------------------------

def add_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument(
        "--root",
        default=os.getcwd(),
        help="repository root to lint (default: from top-level --root or cwd)",
    )
    parser.add_argument(
        "--profile",
        choices=["consumer", "library"],
        default=None,
        help="rule profile: consumer (all-profile rules only) or library (all + library rules)",
    )
    parser.add_argument(
        "--rule",
        dest="rules",
        action="append",
        metavar="RULE_ID",
        help="run only this rule (repeatable)",
    )
    parser.add_argument(
        "--list",
        action="store_true",
        help="list available rules and exit",
    )


def run(args: argparse.Namespace) -> int:
    # Resolve root
    root = getattr(args, "root", None) or getattr(args, "_root_abs", None) or os.getcwd()
    root = os.path.abspath(root)

    # Load config from args._config_dict if available
    config = getattr(args, "_config_dict", {})
    lint_cfg = config.get("lint", {})

    # Determine profile
    profile = getattr(args, "profile", None)
    if profile is None:
        profile = lint_cfg.get("default_profile", "library")

    ctx = Context(root=root, config=config)

    # _LINT_RULES: built-in rules (always available, even in direct test calls).
    # RULES: unified bus — built-in rules re-registered via register(api) plus
    # any external plugin rules. RULES takes priority on collision.
    all_rules = dict(_LINT_RULES)
    all_rules.update(RULES)

    # Apply config severity overrides
    rules_cfg = lint_cfg.get("rules", {})
    effective_rules = {}
    for rid, rule in all_rules.items():
        severity = rules_cfg.get(rid, {}).get("severity", rule.default_severity)
        effective_rules[rid] = (rule, severity)

    # Filter: --list
    if getattr(args, "list", False):
        for rid, (rule, severity) in sorted(effective_rules.items()):
            print(f"  {rid:40s} severity={severity:5s}  profiles={sorted(rule.profiles)}")
        return 0

    # Filter: --rule overrides
    selected_ids = getattr(args, "rules", None)
    if selected_ids:
        effective_rules = {k: v for k, v in effective_rules.items() if k in selected_ids}

    # Filter: by profile
    # "consumer" = only "all"-profile rules; "library" = both "all" and "library"
    def profile_matches(rule: Rule) -> bool:
        if profile == "consumer":
            return "all" in rule.profiles
        # library: run all profiles
        return True

    active_rules = {
        rid: (rule, severity)
        for rid, (rule, severity) in effective_rules.items()
        if severity != "off" and profile_matches(rule)
    }

    # Run rules and collect violations
    any_error = False
    for rid, (rule, severity) in sorted(active_rules.items()):
        violations = rule.check(ctx)
        if not violations:
            continue
        for v in violations:
            path = v["path"]
            line = v["line"]
            detail = v.get("detail", "")
            if detail:
                print(f"{severity.upper()} [{rid}]: {path}:{line}: {detail}")
            else:
                print(f"{severity.upper()} [{rid}]: {path}:{line}")
        count = len(violations)
        print(
            f"check_lint [{rid}]: {count} violation(s) — {rule.hint}",
            file=sys.stderr,
        )
        if severity == "error":
            any_error = True

    if not any_error:
        print("check_lint: all checks passed")
        return 0
    return 1
