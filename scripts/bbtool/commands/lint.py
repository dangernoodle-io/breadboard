"""lint command — rules-based source linter."""
from __future__ import annotations
import argparse
import os
import re
import sys
from pathlib import Path
from typing import List, Optional

from core import Context
from registry import Rule, RULES

NAME = "lint"
HELP = "Run source lint checks"

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
    root = Path(ctx.root)
    comp_root = root / "components"
    if not comp_root.exists():
        return violations

    include_pattern = re.compile(
        r'["<](esp_|freertos/)|["<]driver/|["<]cJSON\.h'
    )
    ifdef_open = re.compile(
        r'^\s*#\s*(?:ifdef\s+ESP_PLATFORM|if\s+defined\s*\(\s*ESP_PLATFORM\s*\))'
    )
    elif_else = re.compile(r'^\s*#\s*(?:elif|else)\b')
    endif_re = re.compile(r'^\s*#\s*endif\b')
    include_re = re.compile(r'^\s*#\s*include\b')

    # Find all public headers, excluding bb_display_ek79007
    for path in sorted(comp_root.glob("*/include/*.h")):
        # Exclude bb_display_ek79007 (any component dir named that)
        parts = path.relative_to(comp_root).parts
        if parts[0] == "bb_display_ek79007":
            continue

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
        r'BB_NET_HEALTH_TOPIC|BB_DIAG_BOOT_TOPIC|BB_UPDATE_CHECK_TOPIC|BB_DISPLAY_INFO_TOPIC'
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


def _check_public_requires_watchlist(ctx: Context) -> list:
    """Rule: public-requires-watchlist — flags watchlist deps in REQUIRES (not PRIV_REQUIRES)."""
    violations = []
    root = Path(ctx.root)
    comp_root = root / "components"
    if not comp_root.exists():
        return violations

    watchlist_prefixes = [
        "esp_driver_", "esp_lcd", "esp_http_server", "esp_timer",
        "esp_system", "app_update", "espressif__mdns",
    ]

    # Allowlist: (component, dep_prefix) pairs
    allowlist = [
        ("bb_display_ek79007", "esp_lvgl_port"),
        ("bb_display_ek79007", "lv_"),
        ("bb_display_ek79007", "lvgl"),
        ("bb_display_ssd1306", "esp_driver_i2c"),
        ("bb_fan_emc2101", "esp_driver_i2c"),
        ("bb_power_tps546", "esp_driver_i2c"),
    ]

    def is_watchlist(dep: str) -> bool:
        for prefix in watchlist_prefixes:
            if dep.startswith(prefix):
                return True
        return False

    def is_allowlisted(comp: str, dep: str) -> bool:
        for (ac, ap) in allowlist:
            if comp == ac and dep.startswith(ap):
                return True
        return False

    for cmake_file in sorted(comp_root.glob("*/CMakeLists.txt")):
        comp = cmake_file.parent.name
        content = ctx.read(cmake_file)
        lines = content.splitlines()
        for i, line in enumerate(lines, 1):
            # Skip PRIV_REQUIRES lines
            if "PRIV_REQUIRES" in line:
                continue
            if "REQUIRES" not in line:
                continue
            # Strip up to and including REQUIRES keyword, strip trailing )
            after = line[line.index("REQUIRES") + len("REQUIRES"):]
            after = after.split(")")[0]
            deps = after.split()
            for dep in deps:
                if dep in ("REQUIRES", "idf_component_register"):
                    continue
                if is_watchlist(dep) and not is_allowlisted(comp, dep):
                    violations.append(ctx.violation(cmake_file, i, f"component={comp} dep={dep}"))
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
    root = Path(ctx.root)
    comp_root = root / "components"
    if not comp_root.exists():
        return violations

    # Read allowlist from config
    rule_cfg = ctx.config.get("lint", {}).get("rules", {}).get(
        "platform-error-in-public-struct", {}
    )
    allowlist: set = set(rule_cfg.get("allow", []))

    for path in sorted(comp_root.glob("*/include/*.h")):
        parts = path.relative_to(comp_root).parts
        if parts[0] == "bb_display_ek79007":
            continue

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
    root = Path(ctx.root)
    comp_root = root / "components"
    if not comp_root.exists():
        return violations

    rule_cfg = ctx.config.get("lint", {}).get("rules", {}).get("bb-prefix", {})
    allowlist: set = set(rule_cfg.get("allow", []))

    for path in sorted(comp_root.glob("*/include/*.h")):
        parts = path.relative_to(comp_root).parts
        if parts[0] == "bb_display_ek79007":
            continue

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
    root = Path(ctx.root)
    comp_root = root / "components"
    if not comp_root.exists():
        return violations

    pragma_re = re.compile(r'^\s*#\s*pragma\s+once\b')

    for path in sorted(comp_root.glob("*/include/*.h")):
        parts = path.relative_to(comp_root).parts
        if parts[0] == "bb_display_ek79007":
            continue

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
    root = Path(ctx.root)
    comp_root = root / "components"
    if not comp_root.exists():
        return violations

    for path in sorted(comp_root.glob("*/include/*.h")):
        parts = path.relative_to(comp_root).parts
        if parts[0] == "bb_display_ek79007":
            continue

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
# Rule: telemetry-rest-cache-read
# ---------------------------------------------------------------------------

# Gather functions that should NOT be called directly in HTTP route handlers;
# REST handlers must read the SSOT snapshot via bb_cache_get_serialized
# (whole-topic) or bb_cache_serialize_into (composed section).
_TELEM_GATHER_FNS = re.compile(
    r'\b(?:bb_wifi_get_info|bb_fan_snapshot|bb_power_snapshot|bb_temp_read_soc)\s*\('
)
# A file is considered a "route handler file" when it has at least one HTTP handler.
_HTTP_HANDLER_SIG = re.compile(r'_handler\s*\(\s*bb_http_request_t')
# A whole-topic handler reads memoized bytes via bb_cache_get_serialized; a
# composed handler embeds a topic section via bb_cache_serialize_into. Either
# counts as a cache-read (the gather-fn call is a documented live fallback).
_CACHE_READ = re.compile(r'\bbb_cache_(?:serialize_into|get_serialized)\s*\(')


def _check_telemetry_rest_cache_read(ctx: Context) -> list:
    """Rule: telemetry-rest-cache-read — REST route handlers must read telemetry
    from the SSOT cache (bb_cache_get_serialized for a whole topic, or
    bb_cache_serialize_into for a composed section) rather than calling gather
    fns (bb_wifi_get_info, bb_fan_snapshot, bb_power_snapshot, bb_temp_read_soc)
    directly.

    A file is flagged when it:
      1. Contains an HTTP handler function signature (_handler(bb_http_request_t).
      2. Calls a telemetry gather function directly.
      3. Does NOT also call a cache-read (bb_cache_get_serialized /
         bb_cache_serialize_into), i.e. has not migrated to the SSOT pattern.
    """
    violations = []
    root = Path(ctx.root)

    for path in ctx.files(
        ["platform/**/*.c", "components/**/*.c"],
        exclude_dirs=[".pio", ".claude"],
    ):
        parts = path.relative_to(root).parts
        if "test" in parts:
            continue

        src = ctx.read(path)
        # Must be a route-handler file.
        if not _HTTP_HANDLER_SIG.search(src):
            continue
        # Only flag if NOT already using cache-read (allows fallback pattern).
        if _CACHE_READ.search(src):
            continue
        # Flag each line that calls a gather fn directly.
        for i, line in enumerate(src.splitlines(), 1):
            if _TELEM_GATHER_FNS.search(line):
                violations.append(ctx.violation(path, i, line.strip()))
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

    kconfig_defaults: dict[str, int] = {}
    for path in ctx.files(
        ["components/**/Kconfig", "platform/**/Kconfig"],
        exclude_dirs=[".pio", ".claude"],
    ):
        for name, base in _parse_kconfig_int_defaults(ctx.read(path)).items():
            kconfig_defaults.setdefault(name, base)

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
            id="public-requires-watchlist",
            default_severity="error",
            profiles={"library"},
            check=_check_public_requires_watchlist,
            hint="move watchlist deps to PRIV_REQUIRES",
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
            id="telemetry-rest-cache-read",
            default_severity="warn",
            profiles={"all"},
            check=_check_telemetry_rest_cache_read,
            hint="REST handlers must read telemetry from the SSOT cache"
                 " (bb_cache_get_serialized / bb_cache_serialize_into)"
                 " rather than calling gather fns directly",
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
