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

[plugins]
paths = []                    # list of .py plugin files (abs or relative to config)
```

`severity = "off"` suppresses the rule entirely. `"warn"` reports violations but exits 0.

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

Consumers that vendor breadboard at `.breadboard/` can run the CLI directly — no cmake
wiring needed in this release.

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

A `bb_lint` cmake target (running `bbtool.py lint --root <consumer> --profile consumer`)
lands with `cmake/bbtool.cmake` in a later PR. For now, call the CLI directly from your
own `make check` or CI step.

## Extending bbtool

Future commands (`version`, `embed`, `gen-site`) and the `cmake/bbtool.cmake` bridge
(which replaces `cmake/bb_version.cmake` + `cmake/bb_embed.cmake`) are coming in
subsequent PRs. The plugin API (`api.add_command(name, module)`) already supports
registering additional commands from plugin files.
