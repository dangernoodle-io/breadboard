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
# Rule registry — register all 4 rules
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
    ]
    for rule in rules:
        _LINT_RULES[rule.id] = rule


_register_lint_rules()


# ---------------------------------------------------------------------------
# Command interface
# ---------------------------------------------------------------------------

def add_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument(
        "--root",
        default=None,
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

    # Merge built-in rules with any plugin rules from RULES registry
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
