# bbtool — breadboard tooling framework

`bbtool` is breadboard's stdlib-only tooling CLI (Python 3.11+, no third-party deps).

```
python3 scripts/bbtool.py <command> [opts]
```

## `lint` command

Run source-level lint checks against a repo tree.

```
python3 scripts/bbtool.py lint [--root DIR] [--profile consumer|library] [--rule ID] [--list]
```

| Flag | Default | Meaning |
|------|---------|---------|
| `--root DIR` | cwd | Repository root to scan |
| `--profile consumer\|library` | `library` (from config) | Rule profile to apply |
| `--rule ID` | (all active rules) | Run only this rule (repeatable) |
| `--list` | — | List available rules and exit |

### Profiles

| Profile | Which rules run | Typical use |
|---------|-----------------|-------------|
| `consumer` | Only `profiles={"all"}` rules — consumer-relevant checks | TaipanMiner, snugfeather, any downstream consumer |
| `library` | All rules (`"all"` + `"library"`) — full component-author checks | breadboard's own `make check` |

### Current rules

| Rule ID | Profile | What it checks |
|---------|---------|----------------|
| `deprecated-http-send` | all | Flags `bb_http_resp_send_json/err/send(` calls in `components/` — use `bb_http_resp_send_chunk` / `bb_http_resp_sendstr` instead |
| `state-topic-post` | all | Flags `bb_event_post(` with a state topic outside `bb_cache/` or `test/` — state topics must flow through `bb_cache` |
| `raw-esp-timer` | all | Flags `esp_timer_create` or `esp_timer_create_args_t` used outside `platform/espidf/bb_timer/` — use `bb_timer_deferred_*` / `bb_timer_worker_*` instead |
| `timer-cb-heavy` | all | Flags heavy work (blocking locks, alloc, IDF-subsystem calls) inside a `bb_timer_(periodic\|oneshot)_create` callback body — use `bb_timer_deferred_*` instead |
| `public-header-leak` | library | Flags `esp_*/driver//cJSON.h` includes in public component headers outside an `#ifdef ESP_PLATFORM` gate |
| `public-requires-watchlist` | library | Flags high-risk ESP-IDF deps (`esp_driver_*`, `esp_lcd`, etc.) in `REQUIRES` when they should be `PRIV_REQUIRES` (allowlist exceptions documented in `check_lint.sh` comments) |
| `platform-error-in-public-struct` | library | Flags integer-typed struct fields in public headers (`components/*/include/*.h`) whose name or trailing comment matches a raw platform error pattern (`esp_err`, `mbedtls`, `tls_*_(err\|code\|fail)`, `disc_reason`, `err_code`, `_errno`) — use a portable `bb_*` enum or keep the field log/diagnostic-only (B1-366) |
| `ticket-ref-in-log` | all | Flags ticket IDs (e.g. `B1-123`, `TA-456`) inside `bb_log_*` runtime string literals across `platform/` and `components/` — reference tickets in comments only, not in log output |
| `bb-prefix` | library | Flags public symbols in `components/*/include/*.h` (function declarations and macros) whose name does not start with `bb_`/`BB_` — all public symbols must carry the library prefix (v0.1.0 convention); configurable allowlist via `[lint.rules.bb-prefix] allow=[...]` |
| `pragma-once` | library | Flags public headers (`components/*/include/*.h`) that do not contain a `#pragma once` line — use `#pragma once` instead of `#ifndef`/`#define` include guards |
| `no-arduino-string` | library | Flags Arduino `String` type usage in library sources (`.c`/`.cpp`/`.h` under `platform/` and `components/`, excluding `.pio`/`.claude`/`test/`) — use `const char*` + length instead |

## `bbtool.toml` config schema

Discovery order: `--config PATH` → `<root>/bbtool.toml` → empty config.

```toml
[lint]
default_profile = "library"   # "consumer" or "library"

[lint.rules.deprecated-http-send]
severity = "error"            # "error" | "warn" | "off"

[lint.rules.public-header-leak]
severity = "error"

[lint.rules.state-topic-post]
severity = "error"

[lint.rules.public-requires-watchlist]
severity = "error"

[lint.rules.raw-esp-timer]
severity = "error"

[lint.rules.timer-cb-heavy]
severity = "error"

[lint.rules.platform-error-in-public-struct]
severity = "warn"             # fires on existing code; set to "error" once clean
allow = []                    # list of field names (or "path:line" strings) to suppress

[lint.rules.ticket-ref-in-log]
severity = "error"
prefixes = ["B1", "TA"]       # ticket-ID prefixes to flag; override for your own tracker

[lint.rules.bb-prefix]
severity = "warn"             # fires on existing chip-register macros; set to "error" once clean
allow = []                    # symbol names (function or macro) exempt from the prefix check

[lint.rules.pragma-once]
severity = "error"

[lint.rules.no-arduino-string]
severity = "error"

[plugins]
paths = []                    # list of .py plugin files (abs or relative to config)
```

`severity = "off"` suppresses the rule entirely. `"warn"` reports violations but exits 0.

### Configurable rule keys

**`platform-error-in-public-struct`**

| Key | Type | Default | Meaning |
|-----|------|---------|---------|
| `allow` | list of strings | `[]` | Field names (or `"path:line"` strings) that are intentional diagnostic/log-only fields and should be suppressed. Example: `allow = ["tls_error_code"]`. |

**`ticket-ref-in-log`**

| Key | Type | Default | Meaning |
|-----|------|---------|---------|
| `prefixes` | list of strings | `["B1", "TA"]` | Ticket-ID prefix(es) to flag inside `bb_log_*` string literals. Override with your own tracker prefixes (e.g. `["JIRA", "PROJ"]`). |

**`bb-prefix`**

| Key | Type | Default | Meaning |
|-----|------|---------|---------|
| `allow` | list of strings | `[]` | Symbol names (function or macro) that are intentional non-`bb_`/`BB_`-prefixed public symbols (e.g. chip-register macros that must match upstream naming). |

## Plugin-authoring contract

A plugin is a `.py` file exposing `def register(api)`. It can register rules (and, in
future, commands) via `api.add_rule(rule)` / `api.add_command(name, module)`.

### `Rule` fields

```python
from registry import Rule

Rule(
    id="my-rule",               # unique string; collision → warn + ignore
    default_severity="error",   # "error" | "warn" | "off"
    profiles={"all"},           # {"all"} or {"library"} or both
    check=my_check_fn,          # fn(ctx) -> list[dict]
    hint="fix suggestion",      # shown in the violation summary line
)
```

### `Context` interface (inside `check(ctx)`)

```python
# Yield Path objects under ctx.root matching globs, skipping excluded dir names
for path in ctx.files(["components/**/*.c"], exclude_dirs=[".pio", ".claude"]):
    ...

# Read a file as UTF-8 (errors='replace')
content = ctx.read(path)

# Build a violation dict
v = ctx.violation(path, lineno, detail="optional extra text")
```

`check(ctx)` returns a list of violation dicts (empty = no violations).

### Minimal worked example

```python
# scripts/my_rules.py
import re
from registry import Rule

def _no_bare_malloc(ctx):
    """Flag bare malloc() calls in component sources."""
    pattern = re.compile(r'\bmalloc\s*\(')
    violations = []
    for path in ctx.files(["components/**/*.c"], exclude_dirs=[".pio"]):
        for i, line in enumerate(ctx.read(path).splitlines(), 1):
            if pattern.search(line):
                violations.append(ctx.violation(path, i, line.strip()))
    return violations

def register(api):
    api.add_rule(Rule(
        id="no-bare-malloc",
        default_severity="warn",
        profiles={"all"},
        check=_no_bare_malloc,
        hint="prefer heap_caps_malloc or bb_mem helpers",
    ))
```

Wire it in `bbtool.toml`:
```toml
[plugins]
paths = ["scripts/my_rules.py"]
```

## Consumer usage (e.g. TaipanMiner)

Consumers that vendor breadboard at `.breadboard/` can run the CLI directly.

```bash
python3 .breadboard/scripts/bbtool.py lint \
    --root . \
    --profile consumer \
    --config bbtool.toml
```

`--profile consumer` runs only the `"all"`-profile rules (consumer-relevant checks) and
skips the `"library"`-profile rules that only apply to breadboard component authors.

### Sample consumer `bbtool.toml`

```toml
[lint]
default_profile = "consumer"

# Relax deprecated-http-send to a warning (consumer uses a wrapper around it)
[lint.rules.deprecated-http-send]
severity = "warn"

# Not applicable to this consumer — turn off entirely
[lint.rules.state-topic-post]
severity = "off"

[plugins]
paths = ["scripts/my_rules.py"]   # consumer-specific rules
```

### cmake convenience target

`cmake/bbtool.cmake` provides a `bb_lint` cmake target that runs
`bbtool.py lint --root <consumer> --profile consumer`. See the **cmake bridge** section
below for wiring.

---

## `version` command

Generates the build-time firmware version string.

```
python3 scripts/bbtool.py version [--emit] [--consumer DIR]
```

**Precedence (highest → lowest):**

1. `BB_FW_VERSION` env var non-empty → used verbatim (override for CI/release pipelines)
2. Consumer repo has an exact git tag at HEAD → use that tag (release builds)
3. Dev default: `dev-<tm-ref>-<bb-ref>`
   - `tm-ref`: `main` on the main branch, else the 7-char short sha; a `+<hash4>` suffix (hash of `git diff`) marks a dirty tree so two dirty checkouts at the same sha stay distinguishable.
   - `bb-ref`: `bb-<pin>` when `.breadboard` is a pinned fetch, or `bb-main`/`bb-<sha>[+hash4]` when `.breadboard` is a local symlink (floating dev checkout).
   - Examples: `dev-main-bb-0.70.3` · `dev-main-bb-main` · `dev-806bf94+a1b2-bb-main`

**Fail-soft:** if git is unavailable, emits `dev-unknown` rather than erroring the build.

### PlatformIO wiring (`scripts/bbtool_pio.py`)

`scripts/bbtool_pio.py` is the canonical breadboard PlatformIO pre-script hook. On each
`pio run` it:
- Calls `bbtool.commands.version` to compute the version string.
- Writes `<PROJECT_DIR>/.breadboard/gen/bb_version_gen.h` containing
  `#define BB_FW_VERSION_STR "<string>"` — only when content changes, preventing spurious
  recompiles.
- Appends `.breadboard/gen` to `CPPPATH` automatically — no manual `build_flags` needed.

Wire in the consumer `platformio.ini`:
```ini
extra_scripts = pre:.breadboard/scripts/bbtool_pio.py
```

**Build-time guarantee:** the script runs on every `pio run`, so the version always
reflects the actual shas at build time even for incremental builds (not configure-time stale).

### CMake wiring

`cmake/bbtool.cmake` sets `PROJECT_VER` from the same logic when included before
`project()`:

```cmake
include("<breadboard>/cmake/bbtool.cmake")
project(my_consumer)
```

This embeds the version into `esp_app_desc.version`.

---

## `embed` command

Compresses a binary file and emits a C source file with a gzipped byte array.

```
python3 scripts/bbtool.py embed <input> <output.c> <symbol>
```

Invoked at CMake configure time by `bb_embed_assets()` (see **cmake bridge** below).
Use directly for one-off asset embedding outside a cmake build.

---

## `gen-site` command

Walks a built SPA `dist/` directory and generates embedded C sources for an HTTP asset
server.

```
python3 scripts/bbtool.py gen-site <dist_dir> <out_dir> <table_sym> [--url-prefix PREFIX]
```

Produces:
- One gzipped `.c` blob per file in `<dist_dir>`.
- A `<table_sym>_table.c` containing a `bb_http_asset_t[]` table + lazy accessor.

Invoked at CMake configure time by `bb_embed_site()` (see **cmake bridge** below).

---

## cmake bridge (`cmake/bbtool.cmake`)

`cmake/bbtool.cmake` is the canonical CMake entry point for all bbtool functions. Include
it **before** `project()`:

```cmake
include("<breadboard>/cmake/bbtool.cmake")
project(my_consumer)
```

### `bb_embed_assets`

Embeds one or more binary assets into firmware sources.

```cmake
bb_embed_assets(
    OUT_SRCS my_asset_srcs
    ASSETS
        path/to/cert.der:cert_der
        path/to/font.bin:font_bin
)
```

Generates at configure time; populate `my_asset_srcs` into your `idf_component_register`
`SRCS`.

### `bb_embed_site`

Embeds an entire SPA dist directory.

```cmake
bb_embed_site(
    OUT_SRCS my_site_srcs
    TABLE    site_assets
    DIST_DIR ${CMAKE_CURRENT_SOURCE_DIR}/dist
    URL_PREFIX /app        # optional
)
```

Generates at configure time; populate `my_site_srcs` into `idf_component_register`
`SRCS`. The resulting `site_assets_table.c` provides the `bb_http_asset_t[]` table for
`bb_http_serve_assets()`.

### `bb_lint` target

`cmake/bbtool.cmake` also registers a `bb_lint` build target equivalent to:
```
python3 .breadboard/scripts/bbtool.py lint --root <consumer> --profile consumer
```

---

## `flash_factory.py` — factory flash post-script

`scripts/flash_factory.py` is a breadboard-provided PlatformIO post-script that registers
a `flash-factory` target. It erases flash and writes `firmware.factory.bin` at offset
`0x0`.

Wire in the consumer `platformio.ini`:
```ini
extra_scripts = post:.breadboard/scripts/flash_factory.py
```

---

## Extending bbtool

The plugin API supports registering additional commands from plugin files via
`api.add_command(name, module)`. Rules are registered via `api.add_rule(rule)` as shown
in the Plugin-authoring contract section above.
