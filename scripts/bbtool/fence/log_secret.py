"""log_secret fence family — repo-wide ratchet-fence for a raw secret-shaped
value reaching a log call outside `bb_log_secret()` (the ONE sanctioned path,
see `components/bb_log/include/bb_log.h`).

This closes the theatre gap in the per-file smoke test
(`test/test_host/test_bb_log_secret.c`): that test greps ONE named file
(`bb_wifi_ap.c`) for two literal substrings, so a differently-worded raw log
line (any label, any file) sails through untouched. This family instead
scans every `bb_log_*`/`ESP_LOG*`/`printf`/`fprintf` call under
`components/`+`platform/` for an `s`-family conversion (`%s`, and the
flag/width/precision-modified forms, e.g. `%-10s`, `%.8s`, `%.*s`) whose
format-string label or corresponding vararg identifier looks secret-shaped,
and fails on ANY such call that does not route through `bb_log_secret()`.

**This is a same-call-site detector, not a taint tracker.** It only ever
looks at the literal call expression it matched -- format string plus the
arguments passed directly in that same call. An over-claiming guard is
worse than a modest one, because it stops people looking: this module
CATCHES:
  - a secret-shaped label or identifier passed directly into a scanned
    call's `%s`-family conversion, including struct-field access
    (e.g. `bb_log_i(TAG, "pw=%s", cfg->password)`), and
  - a multi-line or string-literal-concatenated format string (the label
    token immediately preceding the `%s` conversion is still found even
    when the literal is wrapped across lines or split into adjacent
    `"..." "..."` pieces).

It deliberately does NOT (and cannot, without becoming a real dataflow/
taint analysis -- out of scope for a fence scanner) catch:
  - a secret reached through an intermediate local with a non-secret name,
    e.g. `const char *p = secret; log("x=%s", p)` -- `p` doesn't match the
    secret-name pattern and the scanner never traces `p`'s assignment;
  - an identifier holding a secret whose name contains no listed keyword,
    e.g. `creds.pw` or `ap_key` (neither `pw` nor `ap_key` matches the
    whole-subword keyword list -- see `_SECRET_RE`);
  - a camelCase identifier with no underscore boundary around the keyword,
    e.g. `secretVal` (`_SECRET_RE` matches on `^`/`_` .. `$`/`_` boundaries
    only, by design -- see (b) below); and
  - a secret formatted into a buffer via `snprintf`/similar and logged
    from a separate call -- `snprintf`/`vsnprintf` are not in the scanned
    call set at all (the buffer variable at the eventual log call has, by
    construction, a non-secret name).
Each of these gaps is a known, accepted boundary of a same-call-site
scanner, not an oversight; closing any of them is a cross-variable/taint-
tracking redesign, tracked separately, not attempted here.

Marker types scanned:
  - `log_secret_leak` — a `bb_log_e/w/i/d/v(...)`, `ESP_LOG{E,W,I,D,V}(...)`,
    `printf(...)`, or `fprintf(...)` call whose format string contains an
    `s`-family conversion (`%s`, `%-10s`, `%.8s`, `%.*s`, etc. -- see
    `_S_CONV_RE`; an escaped `%%` immediately followed by a literal `s`
    character is not a conversion and never matches) AND either:
      (a) the format-string label token immediately preceding that
          conversion (e.g. `"password=%s"`, `"AP pass: %s"` ->
          "password"/"pass"), or
      (b) an identifier appearing in one of the trailing (post-format)
          arguments (e.g. `s_ap_password`)
    matches the secret-ish name pattern (password/passwd/pass/psk/secret/
    token/credential/bearer/apikey/api_key, whole-subword, case-insensitive
    -- see `_SECRET_RE`). `bb_log_secret()` itself is never matched (its
    name doesn't fit the `bb_log_[eiwdv]` shape scanned here), and its own
    body's `"%s=%s"` call passes `label`/`value` as arguments -- neither
    matches the pattern, so the sanctioned helper never self-fires.

Deliberately conservative, not a full C parser: comments are stripped
(string-literal-aware) before scanning so a commented-out reference never
counts; parentheses/commas are balanced/split respecting nested calls and
string literals so a multi-line call (e.g. a wrapped format string plus an
`esp_err_to_name(...)` arg) is read as one statement. The identifier check
in (b) scans every identifier token in each trailing argument (not just a
lone variable name), which is a deliberately broad net -- a secret-shaped
substring anywhere in a logged argument's expression is worth a human
looking at, even if this occasionally flags an unrelated identifier that
merely contains one of these words (e.g. a hypothetical `password_hint`
field). Tuned against this repo's actual call sites (see the module's PR):
zero pre-existing false positives at seed time.
"""
from __future__ import annotations
import re
import sys
from pathlib import Path
from typing import List, Optional, Set

from discovery import build_index, ComponentIndex

from fence import _base
from fence._base import Marker

_SCAN_ROOTS = ("components", "platform")
_SRC_GLOBS = ["**/*.c", "**/*.h", "**/*.cpp"]

_BUCKETS = {
    "log_secret_leak": "unmasked secret-shaped log call",
}


def _bucket_for(marker_type: str) -> str:
    return _BUCKETS.get(marker_type, marker_type)


def counts_by_bucket(markers: Set[Marker]) -> dict:
    return _base.counts_by_bucket(markers, bucket_fn=_bucket_for)


# ---------------------------------------------------------------------------
# "owning component" + best-effort enclosing symbol -- same conventions as
# sat_sub.py/clamp.py (see those modules' identity-choice comments for the
# full rationale: `<component>:<enclosing-symbol>:<token>`, never
# `path:line`, so an unrelated edit above a site never trips the fence).
# ---------------------------------------------------------------------------

_CONTROL_KEYWORDS = {"if", "for", "while", "switch", "return", "sizeof", "do", "else"}
_ENCLOSING_FN_RE = re.compile(r'^[A-Za-z_][\w \*]*?\b([A-Za-z_]\w*)\s*\(')
_BARE_SIG_RE = re.compile(r'^([A-Za-z_]\w*)\s*\(')

_FAMILY = "log_secret"


def _component_of(index: ComponentIndex, rel_path: str) -> str:
    """Owning component name for a marker path -- delegates to the
    canonical `discovery.owner_of_path` SSOT (B1-1089; see `sat_sub.py`'s
    twin helper for the full rationale, identical here)."""
    name = index.owner_of_path(rel_path)
    if name is not None:
        return name
    return _base.resolve_owner_fallback(_FAMILY, rel_path)


def _enclosing_symbol(lines: List[str], line_idx: int) -> str:
    for i in range(line_idx, -1, -1):
        line = lines[i]
        if not line or line[0].isspace():
            continue
        m = _ENCLOSING_FN_RE.match(line)
        if m and m.group(1) not in _CONTROL_KEYWORDS:
            return m.group(1)
        m2 = _BARE_SIG_RE.match(line)
        if m2 and m2.group(1) not in _CONTROL_KEYWORDS and not line.rstrip().endswith(";"):
            return m2.group(1)
    return "?"


# ---------------------------------------------------------------------------
# Comment stripping -- string-literal-aware, so `//`/`/* */` inside a string
# is never mistaken for a real comment, and a genuine comment (including one
# that happens to contain a raw-looking log call) never contributes a
# marker. Preserves length/newlines (comment bytes become spaces) so line
# numbers used for `_enclosing_symbol` stay accurate.
# ---------------------------------------------------------------------------

def _strip_comments(text: str) -> str:
    out: List[str] = []
    i = 0
    n = len(text)
    in_line_comment = False
    in_block_comment = False
    in_str = False
    str_char = ""
    while i < n:
        c = text[i]
        if in_line_comment:
            if c == "\n":
                in_line_comment = False
                out.append(c)
            else:
                out.append(" ")
            i += 1
            continue
        if in_block_comment:
            if c == "*" and i + 1 < n and text[i + 1] == "/":
                out.append("  ")
                i += 2
                in_block_comment = False
                continue
            out.append("\n" if c == "\n" else " ")
            i += 1
            continue
        if in_str:
            out.append(c)
            if c == "\\" and i + 1 < n:
                out.append(text[i + 1])
                i += 2
                continue
            if c == str_char:
                in_str = False
            i += 1
            continue
        if c == '"' or c == "'":
            in_str = True
            str_char = c
            out.append(c)
            i += 1
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            in_line_comment = True
            out.append("  ")
            i += 2
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            in_block_comment = True
            out.append("  ")
            i += 2
            continue
        out.append(c)
        i += 1
    return "".join(out)


# ---------------------------------------------------------------------------
# Balanced-paren / top-level-comma helpers -- string/char-literal aware, so
# a call like `bb_log_e(TAG, "%s=%s", esp_err_to_name(err), s)` reads as
# exactly 3 top-level arguments, not confused by the nested call's parens.
# ---------------------------------------------------------------------------

def _find_matching_paren(text: str, open_idx: int) -> int:
    depth = 0
    i = open_idx
    n = len(text)
    in_str = False
    str_char = ""
    while i < n:
        c = text[i]
        if in_str:
            if c == "\\":
                i += 2
                continue
            if c == str_char:
                in_str = False
            i += 1
            continue
        if c == '"' or c == "'":
            in_str = True
            str_char = c
            i += 1
            continue
        if c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return -1


def _split_top_level_args(arg_text: str) -> List[str]:
    args: List[str] = []
    depth = 0
    in_str = False
    str_char = ""
    cur: List[str] = []
    i = 0
    n = len(arg_text)
    while i < n:
        c = arg_text[i]
        if in_str:
            cur.append(c)
            if c == "\\" and i + 1 < n:
                cur.append(arg_text[i + 1])
                i += 2
                continue
            if c == str_char:
                in_str = False
            i += 1
            continue
        if c == '"' or c == "'":
            in_str = True
            str_char = c
            cur.append(c)
            i += 1
            continue
        if c in "([{":
            depth += 1
            cur.append(c)
            i += 1
            continue
        if c in ")]}":
            depth -= 1
            cur.append(c)
            i += 1
            continue
        if c == "," and depth == 0:
            args.append("".join(cur).strip())
            cur = []
            i += 1
            continue
        cur.append(c)
        i += 1
    tail = "".join(cur).strip()
    if tail:
        args.append(tail)
    return args


def _extract_string_literal(arg_text: str) -> str:
    """Concatenate every double-quoted literal segment found in `arg_text`
    -- handles adjacent-literal concatenation (`"a" "b"`), which C treats as
    one string. Non-literal expressions simply contribute no text."""
    parts: List[str] = []
    i = 0
    n = len(arg_text)
    while i < n:
        if arg_text[i] == '"':
            j = i + 1
            buf: List[str] = []
            while j < n and arg_text[j] != '"':
                if arg_text[j] == "\\" and j + 1 < n:
                    buf.append(arg_text[j + 1])
                    j += 2
                    continue
                buf.append(arg_text[j])
                j += 1
            parts.append("".join(buf))
            i = j + 1
        else:
            i += 1
    return "".join(parts)


# ---------------------------------------------------------------------------
# Call-shape recognition + secret-name matching.
# ---------------------------------------------------------------------------

_CALL_RE = re.compile(r'\b(bb_log_[eiwdv]|ESP_LOG[EWIDV]|printf|fprintf)\s*\(')

# Whole-subword match: bounded by `^`/`_` and `$`/`_`, so `s_ap_password`
# matches on its "password" subword, but `passed`/`class`/`token_registry`
# (a plain identifier with no underscore boundary around the substring) do
# not. `pass` is deliberately last among the password-family alternatives so
# a longer, more specific alternative (`password`, `passwd`) doesn't matter
# for correctness here (whole-subword matching makes ordering irrelevant),
# but keeping the specific spellings first documents intent.
_SECRET_RE = re.compile(
    r'(?:^|_)(password|passwd|pass|psk|secret|token|credential|bearer|apikey|api_key)(?:$|_)',
    re.IGNORECASE,
)

# Matches an identifier run immediately followed only by `:`, `=`, or
# whitespace before the format string's `%s`-family conversion -- e.g.
# "password=" or "pass: " or "pass ". A conversion with no such trailing
# label shape (e.g. directly after `(` or `)`, like `"...stop() (%s)"`)
# yields no label match, leaving the trailing-argument-identifier check as
# the only path.
_LABEL_BEFORE_PCT_S_RE = re.compile(r'([A-Za-z_][A-Za-z0-9_]*)[:=\s]*$')

_IDENT_RE = re.compile(r'[A-Za-z_][A-Za-z0-9_]*')

# An escaped `%%` is a literal percent sign, never the start of a
# conversion -- mask every `%%` pair to two spaces (same length, so match
# offsets used for label lookback stay valid) before scanning for
# conversions, so `"%%s"` (literal "%s" text, not a format directive) never
# mismatches as an `s`-conversion.
_ESCAPED_PCT_RE = re.compile(r'%%')

# `s`-family conversion: `%` + optional flags (`-+0 #`) + optional width
# (digits or `*`) + optional `.precision` (digits or `*`) + the `s` type
# char. Matches plain `%s` as well as `%-10s`, `%.8s`, `%.*s`, etc.
_S_CONV_RE = re.compile(r'%[-+0 #]*(?:\d+|\*)?(?:\.(?:\d+|\*))?s')


def _mask_escaped_percent(fmt_str: str) -> str:
    return _ESCAPED_PCT_RE.sub("  ", fmt_str)


def _fmt_arg_index(fn: str) -> int:
    if fn == "printf":
        return 0
    # bb_log_[eiwdv], ESP_LOG[EWIDV], fprintf all carry (tag_or_stream, fmt, ...)
    return 1


def _find_secret_token(fmt_str: str, trailing_args: List[str]) -> Optional[str]:
    masked = _mask_escaped_percent(fmt_str)
    for m in _S_CONV_RE.finditer(masked):
        pre = fmt_str[: m.start()]
        lm = _LABEL_BEFORE_PCT_S_RE.search(pre)
        if lm and _SECRET_RE.search(lm.group(1)):
            return lm.group(1).lower()
    for arg in trailing_args:
        for idm in _IDENT_RE.finditer(arg):
            ident = idm.group(0)
            if _SECRET_RE.search(ident):
                return ident.lower()
    return None


def _scan_log_secret(root: Path) -> Set[Marker]:
    found: Set[Marker] = set()
    index = build_index([str(root)])
    for path in _base.iter_files(root, _SCAN_ROOTS, _SRC_GLOBS):
        rel = _base.rel(root, path)
        text = _base.read(path)
        clean = _strip_comments(text)
        lines = clean.splitlines()
        for m in _CALL_RE.finditer(clean):
            fn = m.group(1)
            open_idx = m.end() - 1
            close_idx = _find_matching_paren(clean, open_idx)
            if close_idx == -1:
                continue
            args = _split_top_level_args(clean[open_idx + 1:close_idx])
            fmt_i = _fmt_arg_index(fn)
            if fmt_i >= len(args):
                continue
            fmt_str = _extract_string_literal(args[fmt_i])
            if not _S_CONV_RE.search(_mask_escaped_percent(fmt_str)):
                continue
            trailing_args = args[fmt_i + 1:]
            token = _find_secret_token(fmt_str, trailing_args)
            if token is None:
                continue
            line_idx = clean.count("\n", 0, m.start())
            component = _component_of(index, rel)
            symbol = _enclosing_symbol(lines, line_idx)
            found.add(Marker("log_secret_leak", rel, f"{component}:{symbol}:{token}"))
    return found


def scan_all(root: str) -> Set[Marker]:
    return _base.scan_all(sys.modules[__name__], root)
