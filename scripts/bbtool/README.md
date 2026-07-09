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
| `public-requires-unused` | library | Flags a platform/third-party `REQUIRES` dep (any token not starting with `bb_`) not referenced by any public header (`include/*.h`) — decisive test from CLAUDE.md's "REQUIRES vs PRIV_REQUIRES" section; internal `bb_*`-to-`bb_*` deps are out of scope. Default severity `warn` (a known unfixed `bb_wifi`/`esp_netif` leak would otherwise break CI); allowlist via `[lint.rules.public-requires-unused] allow=[[comp, dep], ...]` |
| `platform-error-in-public-struct` | library | Flags integer-typed struct fields in public headers (`components/*/include/*.h`) whose name or trailing comment matches a raw platform error pattern (`esp_err`, `mbedtls`, `tls_*_(err\|code\|fail)`, `disc_reason`, `err_code`, `_errno`) — use a portable `bb_*` enum or keep the field log/diagnostic-only (B1-366) |
| `ticket-ref-in-log` | all | Flags ticket IDs (e.g. `B1-123`, `TA-456`) inside `bb_log_*` runtime string literals across `platform/` and `components/` — reference tickets in comments only, not in log output |
| `bb-prefix` | library | Flags public symbols in `components/*/include/*.h` (function declarations and macros) whose name does not start with `bb_`/`BB_` — all public symbols must carry the library prefix (v0.1.0 convention); configurable allowlist via `[lint.rules.bb-prefix] allow=[...]` |
| `pragma-once` | library | Flags public headers (`components/*/include/*.h`) that do not contain a `#pragma once` line — use `#pragma once` instead of `#ifndef`/`#define` include guards |
| `no-arduino-string` | library | Flags Arduino `String` type usage in library sources (`.c`/`.cpp`/`.h` under `platform/` and `components/`, excluding `.pio`/`.claude`/`test/`) — use `const char*` + length instead |
| `component-readme` | library | Flags `components/<name>/` directories with no `README.md` (see the `docs` command below for the README template). Default severity `warn` — fires broadly on undocumented components today by design; flips to `error` once the fill lands (B1-646) |
| `kconfig-bridge-shadow` | all | Flags a bare `#ifndef BB_X`/`#define BB_X <literal>` C fallback for a name X that also has a `config BB_X` int declared in Kconfig, when the same file has no `CONFIG_BB_X` bridge tying the C macro to the Kconfig symbol — the knob is silently inert (shipped 3×, see CLAUDE.md "Avoiding audit-class regressions") |
| `raw-timestamp-divide` | all | Flags raw millisecond conversions (`esp_timer_get_time()/1000` or `bb_timer_now_us()/1000`, any C integer suffix) that bypass the canonical `bb_clock` helper, outside the real `bb_clock.c`/`bb_clock.h` files and any `bb_timer/` component directory — use `bb_clock_now_ms64()`/`bb_clock_now_ms()` instead. Default severity `warn`; allowlist via `[lint.rules.raw-timestamp-divide] allow=[...]` |

## `fence` command

Unified ratchet-fence lint over one or more marker **families**. A family
(`scripts/bbtool/fence/<family>.py`) is a group of `_scan_*` marker-detection
functions plus its own committed baseline at
`.baseline/bbtool/fence/<family>.json`. Five families exist today:
`di_legacy` (breadboard's legacy DI/self-registration glue surface), `clamp`
(hand-rolled reimplementations of `bb_num`'s two-sided clamp),
`scalar_parse` (hand-rolled reimplementations of `bb_scalar`'s parsers),
`sat_sub` (hand-rolled one-sided saturating-subtract idioms), and
`new_component` (the "no unauthorized components" guardrail — see below) —
one family per shared idiom/helper/surface, so a future one is a new
module, not a combined-family rewrite.

```
python3 scripts/bbtool.py fence [--root DIR] [--family NAME ...] [--update-baseline] [--seed FAMILY] [--approve COMPONENT]
```

| Flag | Default | Meaning |
|------|---------|---------|
| `--root DIR` | cwd | Repository root to scan |
| `--family NAME` | all discovered families | Restrict to this family (repeatable) |
| `--update-baseline` | — | **Shrink-only**: prune baseline entries whose occurrence no longer exists; never adds a net-new occurrence (a fresh duplicate always stays a failure) |
| `--seed FAMILY` | — | One-time: bless FAMILY's current occurrence set as its starting baseline; errors if a baseline already exists (`--update-baseline` is what you want after that) |
| `--approve COMPONENT` | — | `new_component` family only: append the single named component (must already exist at `components/COMPONENT/`) to the `new_component` baseline — the sanctioned grow-by-approval path; mutually exclusive with `--seed`/`--update-baseline`/`--family` and never touches any other family's baseline |

**What it does.** For each targeted family it scans the tree for that
family's marker types, diffs the current occurrence set against the
family's committed baseline, and **fails on any net-new occurrence** not
already in the baseline. Removals never fail — they are reported as `INFO
"candidate to prune"` so the baseline can shrink over time. New composition
never needs a new occurrence of a fenced marker — fenced surfaces are
frozen glue being phased out, not a sanctioned extension point.

**Identity is symbol-keyed, not path-keyed** (per family, via an optional
`identity(marker)` override — see `di_legacy`'s pub-sink handling). A
marker's default ratchet-diff identity is `(marker_type, symbol)`; the file
path is retained only as informational metadata. A pure file/component
rename does not trip the fence (no spurious remove+add); only a genuinely
new or removed symbol does.

**`--update-baseline` is shrink-only, not a blanket overwrite.** It prunes
entries no longer found in the tree, but a net-new occurrence is never
silently added to the baseline — that always requires either removing the
occurrence or a deliberate, reviewed baseline edit. This is stricter than a
"regenerate from current scan" mode: a copy-pasted duplicate of an already-
fenced marker can never be blessed by accident.

**Seeding a brand-new family** is a separate one-time path
(`--seed FAMILY`): it writes the family's current occurrence set as its
starting baseline, refusing to run if a baseline already exists (use
`--update-baseline` for maintenance after that point).

### `di_legacy` family

The original ratchet fence, unchanged: freezes breadboard's legacy
dependency-injection glue surface (self-registration macros,
autoregister/auto-attach Kconfig options, pub-captive-sink patterns,
force-keep linker directives). Scans `components/` + `platform/` for
BB_INIT_REGISTER family, `*_AUTOREGISTER`/`*_AUTO_ATTACH` Kconfig defs +
usages, `bb_pub_sink_t`/`bb_pub_add_sink`, display force-keep, and
`bb_init_force_register*` CMake calls — see
`scripts/bbtool/fence/di_legacy.py`.

**Scope: ESP-IDF + host only.** The Arduino backend
(`platform/arduino/bb_display_*`) self-registers display backends via a
`__attribute__((constructor))` function definition
(`bb_display_register__<chip>`), a different mechanism than the ESP-IDF
`BB_DISPLAY_AUTOREGISTER` macro / `-u` linker force-keep this fence scans
for. That Arduino path is **not currently scanned** — Arduino is behind
ESP-IDF on platform parity and not under active development, so scanner
investment there is deliberately deferred rather than silent; see the
`KNOWN GAP` comment above `_scan_display_force_keep` in
`scripts/bbtool/fence/di_legacy.py`.

### `clamp` family

Freezes hand-rolled reimplementations of `bb_num`'s two-sided numeric clamp
(`bb_clampi`/`bb_clampf`). Scans `components/` + `platform/` for a hand-rolled
two-sided clamp: if-pair (`if (x < lo) x = lo; if (x > hi) x = hi;`, any
`</<=`+`>/>=` combination), nested ternary (`x < lo ? lo : (x > hi ? hi :
x)`), or MIN/MAX nesting (`MAX(lo, MIN(hi, x))` / `fmaxf`/`fminf` /
`std::max`/`std::min`). A **one-sided** saturating op (e.g. bb_ring's
underflow-clamp-at-0, or bb_task_resolve's single-bound unicore-affinity
fallback) deliberately does **not** match — it has no second,
opposite-direction bound to reimplement bb_clampi/bb_clampf's actual job
(the `sat_sub` family below is the home for fencing that one-sided idiom,
not this one). The canonical impl (`platform/host/bb_num/`) is
excluded. Identity is `<component>:<enclosing-symbol>:<var>` (best-effort,
no real C parser — see `clamp.py`'s identity-choice comment), not
`path:line`, so an unrelated edit above a clamp never trips the fence. See
`scripts/bbtool/fence/clamp.py`.

### `scalar_parse` family

Guards against hand-rolled reimplementations of `bb_scalar`'s strict scalar
parsers (`bb_scalar_parse_bool`/`bb_scalar_parse_uint`, which superseded
bb_http_server's former `bb_url_parse_bool`/`bb_url_parse_uint`). Scans
`components/` + `platform/` for a **definition** of `bb_url_parse_bool`/
`bb_url_parse_uint` outside `bb_scalar`. The baseline is fully drained and
locked: reintroducing either symbol (or a hand-rolled duplicate under those
names) outside `bb_scalar` is blocked. Symbol-keyed (id = the function
name); an accepted limitation is that this catches reintroduction of these
two named symbols only, not arbitrary inline parsing that duplicates their
behavior under a different name. See `scripts/bbtool/fence/scalar_parse.py`.

### `sat_sub` family

Freezes hand-rolled one-sided saturating-subtract idioms (a subtract
floored at 0 to avoid unsigned/negative underflow, e.g. `used = (total >
free) ? (total - free) : 0`). There is no canonical extracted helper for
this idiom yet (extraction is parked); this fence exists to stop the
hand-rolled duplication from growing further while extraction is deferred.
Scans `components/` + `platform/` for two shapes:

- the ternary/guard-expression shape (three variants): forward (`(A > B) ?
  (A - B) : 0`, or `>=`), reversed (`(A < B) ? 0 : (A - B)`, or `<=`), and
  the decrement-by-1 special case (`(A > 0) ? A - 1 : 0`, e.g. stripping a
  NUL terminator's length). `B` may be an identifier or a numeric literal;
  an optional cast before the subtraction is allowed.
- the post-hoc delta-then-clamp shape: `X = <expr containing a
  subtraction>; if (X < 0) X = 0;`, single-line or braced (e.g. `int64_t
  elapsed_us = now_us - last_us; if (elapsed_us < 0) { elapsed_us = 0; }`).
  Precision guard: only fires when the line immediately preceding the `if
  (X < 0)` guard is itself an assignment to that same var whose
  right-hand side contains a subtraction — this is what distinguishes the
  idiom from the far more common bare negative-error-code guard (`if (n <
  0) return false;`), which never matches.

Deliberately does **not** match a two-sided clamp (`clamp`'s job) or a
bounded-chunk loop (`remaining -= chunk` where `chunk` was already computed
as `min(remaining, N)` — no separate underflow guard being reimplemented
there). `platform/host/bb_num/` is excluded, same as `clamp`. Identity is
`<component>:<enclosing-symbol>:<var>` (best-effort, no real C parser —
same convention as `clamp.py`), not `path:line`. See
`scripts/bbtool/fence/sat_sub.py`.

### `new_component` family — grow-by-approval, not shrink-only

Every other family above is a **drain**: it freezes an existing surface and
only ever shrinks as sites migrate off it. `new_component` is the opposite
shape — it's a **guardrail on creation**, per the component-creation policy
(breadboard `CLAUDE.md` "Component creation" + wiki
[design/Component-Taxonomy#when-to-create-a-new-component](https://github.com/dangernoodle-io/breadboard/wiki/design/Component-Taxonomy#when-to-create-a-new-component),
KB #402): extending an existing component is the default, and a genuinely
new `components/<name>/` needs a distinct dependency, a real consumer, and
reviewed design sign-off — never a speculative/ad-hoc add.

Scans `components/` for every directory directly under it (one `component`
marker per directory, id = the component name; `components/README.md` and
similar files are not markers). A directory already in the baseline is
fine; a **new, unapproved** `components/<name>/` is exactly the case this
fence exists to catch, so it fails the fence, by design, until a human
deliberately approves it. A **removed** component (e.g. the bb_pub_wifi
deletion) prunes normally via `--update-baseline`, same as any other
family — the guardrail is only on the create direction.

**Approving a new component** goes through `--approve COMPONENT`, not
`--update-baseline` — `--update-baseline` stays shrink-only for every
family, this one included, and never blesses a net-new occurrence.
`--approve` is a distinct, narrowly-scoped path: it verifies
`components/COMPONENT/` actually exists on disk, then appends exactly that
one entry to `new_component`'s baseline — nothing else in the tree, and no
other family's baseline, is touched. The resulting one-line diff on
`.baseline/bbtool/fence/new_component.json` *is* the reviewed design
sign-off; it lands in the same PR as the new component, through a
validated, mistake-resistant command rather than a hand-edited JSON file.

```
python3 scripts/bbtool.py fence --approve bb_new_thing
```

See `scripts/bbtool/fence/new_component.py` (scanner) and
`scripts/bbtool/commands/fence_cmd.py` (`--approve` wiring).

### `di-fence` command (back-compat alias)

`python3 scripts/bbtool.py di-fence [--root DIR] [--update-baseline]` is a
thin alias for `fence --family di_legacy` — same flags, same
pass/fail/shrink-only semantics — kept so existing scripts/muscle-memory
keep working. New usage should prefer `fence --family di_legacy` (or just
`fence`, since it covers every discovered family).

### Adding a fence family

Turnkey, no manual registry-list edit:

1. Add `scripts/bbtool/fence/<family>.py` (any filename not starting with
   `_`) — the module name becomes the family name.
2. Define one or more `_scan_<name>(root) -> Set[Marker]` functions; they
   are auto-collected (see `fence/_base.py:discover_scanners`) — no
   `scan_all()` to hand-maintain.
3. Optionally define `identity(marker) -> tuple` to override the default
   `(type, id)` ratchet-diff identity (see `di_legacy.identity` for the
   path-insensitive pub_sink example).
4. Run `python3 scripts/bbtool.py fence --seed <family>` once to write the
   starting baseline to `.baseline/bbtool/fence/<family>.json`; commit it.
5. `fence` (and `make fence` / `make check`) now covers the new family
   automatically alongside the existing ones.

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

[lint.rules.public-requires-unused]
severity = "warn"
allow = []                    # additional [comp, dep] pairs beyond the built-in default allowlist

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

[lint.rules.component-readme]
severity = "warn"             # fires broadly on undocumented components today; flip to "error" once the fill lands (B1-646)

[lint.rules.kconfig-bridge-shadow]
severity = "error"

[lint.rules.raw-timestamp-divide]
severity = "warn"
allow = []                    # list of path strings or "path:line" keys to suppress

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

### Authoring conventions

- Reuse shared parsers — parse CMake `REQUIRES`/`PRIV_REQUIRES` via `cmake_parse.parse_requires`, never hand-roll REQUIRES scanning.
- Config-driven, not hardcoded — a rule's severity and allowlists come from `bbtool.toml` (`[lint.rules.<id>]`); don't bake exceptions into the check body.

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

## `docs` command

Regenerates the GENERATED marker regions inside component READMEs, and can
scaffold a brand-new component README from a stampable template.

```
python3 scripts/bbtool.py docs gen [--root DIR]
python3 scripts/bbtool.py docs scaffold <component> [--root DIR]
```

There is no `--check` mode. The drift gate is `bbtool docs gen` followed by
`git diff --exit-code` — wired into `make check` as the `docs-check` target.
CI fails if `gen` produces a diff (i.e. someone edited a component or its
CMakeLists without re-running `docs gen` and committing the result).

### Per-component README template

**`scripts/bbtool/templates/component-readme.md` is the single source of
truth** for the shape of a component README. It is a generic, prefix-
parameterized template — no `bb_`/GitHub/Doxygen literals — stamped by
`docs scaffold` (below). Component READMEs (`bb_attrs` / `bb_filter` are the
worked examples) follow that skeleton; read the template file directly rather
than duplicating its prose here.

### `docs scaffold <component>` subcommand

```
python3 scripts/bbtool.py docs scaffold bb_foo
```

Stamps `templates/component-readme.md` into `components/<component>/README.md`,
substitutes tokens, then immediately runs the same marker-region generation as
`docs gen` over the fresh file — a scaffolded README lands fully populated
(brief/api/deps/platform/links/wiring sections filled in) in one shot, no
separate `docs gen` call needed.

**Never overwrites.** If `components/<component>/README.md` already exists,
`docs scaffold` errors loudly to stderr and exits non-zero without touching
the file.

**Template tokens:**

| Token | Source | Notes |
|-------|--------|-------|
| `{{component}}` | the `<component>` argument | e.g. `bb_foo` |
| `{{prefix}}` | derived from `{{component}}` | text before the first `_`, e.g. `bb` |

The template has no optional token-substituted regions left — the Links
section (repo URL / wiki links) is now a `bbtool:links` marker region,
populated by the same `docs gen` pass as `brief`/`api`/`deps`/`platform`/
`wiring` (below), from `[docs]` config regardless of whether it is set (an
absent `[docs]` block renders `_(no links configured)_` rather than omitting
the section).

### `[docs]` config block (`bbtool.toml`)

Optional. Absent ⇒ the generated Links section renders `_(no links
configured)_` and the deps table's per-dep "Role"/"Docs" cells fall back to
"—" / the components index for deps with no README.

```toml
[docs]
repo_url = "https://github.com/<org>/<repo>"
wiki_base = "https://github.com/<org>/<repo>/wiki"
links = ["https://github.com/<org>/<repo>/wiki/Component-Docs"]

[docs.component_links]
bb_foo = ["https://github.com/<org>/<repo>/wiki/foo-notes"]
```

| Key | Type | Meaning |
|-----|------|---------|
| `repo_url` | string | Repository URL; rendered as a "Repository:" link in every component README's generated Links section |
| `wiki_base` | string | Wiki base URL. Also the source of each component's own primary wiki link, derived as `<wiki_base>/components/<name>` (the wiki `components/` subdir convention) — there is no per-component C-header doc-link annotation channel in this codebase |
| `links` | list of strings | Global extra links, applied to every component README's Links section |
| `[docs.component_links]` | table of string→list of strings | Component name → extra links for that component only |

The generated Links section merges, in order, the component's own
`wiki_base/components/<name>` link, `[docs].links` (global), then
`[docs.component_links].<name>` (per-component) — deduplicated by URL, first
occurrence wins. Badges are not part of component README generation
(top-level project README badges are hand-authored, out of scope for
`bbtool docs`).

**Marker convention (terraform-docs style).** `docs gen` only rewrites the
text strictly between a `<!-- BEGIN bbtool:<key> -->` / `<!-- END
bbtool:<key> -->` pair; everything else in the file — hand-authored prose,
other sections, marker lines themselves — is left byte-for-byte untouched. A
README with no markers is never modified, and `docs gen` never creates or
scaffolds a README for a component that doesn't have one — component READMEs
are hand-authored from birth (see `CLAUDE.md`); `docs gen` only fills in the
generated regions of READMEs that already opt in.

Seven marker keys are generated today:

| Key | Source | Renders |
|-----|--------|---------|
| `brief` | The component's primary public header's FIRST Doxygen `@brief` (`components/<name>/include/<name>.h`, via `header_annot.extract_brief`) | The component's one-line purpose prose |
| `api` | `components/<name>/include/*.h` | A linked list of public headers, plus a line naming the component's `<prefix>_` symbol prefix |
| `deps` | `components/<name>/CMakeLists.txt`'s `idf_component_register(...)` `REQUIRES` + `PRIV_REQUIRES` args (direct deps only, no transitive walk) | A `Component \| Kind \| Role \| Docs` table, sorted, one row per dep — "Role" is the dep's own README first sentence (or "—"), "Docs" links to the dep's own `README.md` (or falls back to `components/README.md`) |
| `platform` | Presence of `platform/{host,espidf,arduino}/<name>/` directories | A host/espidf/arduino yes/no matrix |
| `links` | `[docs].wiki_base` (self link) + `[docs].links` (global) + `[docs.component_links].<name>` (per-component), plus `[docs].repo_url` | A deduplicated bullet list of links |
| `wiring` | The component name | A one-line pointer to the wiki's "use in your project" guide for this component |
| `budget` | Every committed `.baseline/bbtool/metrics/*.json` (see the `size` command above) that lists the component in its `flash.components` map | A `Target \| flash \| Δ vs baseline` table, sorted by target (Δ is always "—" — a static committed value, not a live compare); gains `min_free`/`high_water` columns once any baseline's `heap` block is populated. FAIL-SOFT: renders `_(no baseline)_` when no baseline matches, rather than erroring |

**Config knobs stays hand-authored.** Kconfig-driven generation for this
section is a later fork (parsing `Kconfig` files is explicitly out of scope
here) — document known knobs by hand until then.

**Determinism.** Both generators sort their output and never leak dict/set
iteration order, embed timestamps, or embed absolute paths; a second `docs
gen` run on unchanged inputs produces zero diff.

### `component-readme` lint rule

The `component-readme` rule (see the `lint` command's rule table above) flags
any `components/<name>/` directory with no `README.md`. It runs at `warn`
severity — deliberately non-blocking, since a large majority of components
have no README today pending the taxonomy decision (B1-644) and open forks;
a later PR flips it to `error` once the fill lands (B1-646).

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

## `fetch` command

Reconciles the `.breadboard` directory against a pinned version tag or a local
checkout override.

```
python3 scripts/bbtool.py fetch --dest DEST --version VERSION [--repo REPO] [--local LOCAL]
```

| Flag | Default | Meaning |
|------|---------|---------|
| `--dest DEST` | (required) | Path to the `.breadboard` directory |
| `--version VERSION` | (required) | breadboard version tag, branch name, or full 40-char commit SHA to pin (see below) |
| `--repo REPO` | `https://github.com/dangernoodle-io/breadboard.git` | Git remote to clone from |
| `--local LOCAL` | `$BREADBOARD_LOCAL` | Local breadboard checkout to symlink; overrides clone |

**`--version` accepts a release tag, a branch name, or a full 40-character lowercase hex commit SHA.**
Short SHAs are not supported.
Tags and branches use a fast `git clone --depth 1 --branch` shallow clone.
A full 40-char SHA uses a shallow fetch-by-commit (`git init` + `git fetch --depth 1 origin <sha>` + `git checkout FETCH_HEAD`), which requires the remote to allow fetching reachable SHAs — GitHub does.
For releases, a tag is still preferred over a raw SHA.

### `reconcile()` function

```python
from commands.fetch import reconcile

reconcile(dest, version, repo=DEFAULT_REPO, local=None)
```

Implements four branches in priority order:

1. **LOCAL set** → ensure `DEST` is a symlink to `abspath(LOCAL)`. Replaces any
   existing directory or wrong-target symlink.
2. **DEST is a symlink** (prior local build, no LOCAL override now) → leave as-is;
   prints the real target.
3. **DEST/components is a dir AND stamp matches VERSION** → up to date; noop.
4. **Missing or stale** → delete `DEST` if present, then fetch at VERSION:
   - **tag / branch**: `git clone --depth 1 --branch VERSION REPO DEST`
   - **full 40-char SHA**: `git init` + `git remote add origin REPO` + `git fetch --depth 1 origin SHA` + `git checkout FETCH_HEAD`

   Write a `.version` stamp file in both cases.

### Standardized consumer bootstrap stub

Cold-start (clone-if-missing / symlink-if-BREADBOARD_LOCAL) is irreducible and must
live in a consumer-side pre-script that runs before `bbtool` is importable.
The cold-start handles a tag, branch, or full 40-char commit SHA (matching `bbtool fetch`'s
`--version`); short SHAs are not supported.
Everything after that first-time bootstrap delegates to `reconcile()`.

The canonical consumer stub is `scripts/fetch_breadboard.py`:

```python
"""Bootstrap breadboard at the pinned VERSION, then delegate reconcile to bbtool.
Standardized stub — the only value to edit is VERSION (and REPO if you fork).
Wire as: extra_scripts = pre:scripts/fetch_breadboard.py
See breadboard scripts/bbtool/README.md."""
Import("env")  # PlatformIO SCons pre-script
import os, sys, subprocess

VERSION = "v0.0.0"  # breadboard pin
REPO = "https://github.com/dangernoodle-io/breadboard.git"
DEST = os.path.join(env.subst("$PROJECT_DIR"), ".breadboard")
LOCAL = os.environ.get("BREADBOARD_LOCAL")

# Cold start: make a .breadboard exist so bbtool is importable. Minimal + irreducible.
import re as _re
if LOCAL and not os.path.exists(DEST):
    os.symlink(os.path.abspath(LOCAL), DEST)
elif not LOCAL and not os.path.exists(DEST):
    if _re.fullmatch(r"[0-9a-f]{40}", VERSION):
        os.makedirs(DEST)
        subprocess.check_call(["git", "-C", DEST, "init", "-q"])
        subprocess.check_call(["git", "-C", DEST, "remote", "add", "origin", REPO])
        subprocess.check_call(["git", "-C", DEST, "fetch", "--depth", "1", "origin", VERSION])
        subprocess.check_call(["git", "-C", DEST, "checkout", "-q", "FETCH_HEAD"])
    else:
        subprocess.check_call(["git", "clone", "--depth", "1", "--branch", VERSION, REPO, DEST])

# Delegate the rest (stamp reconcile, stale refetch, symlink idempotency) to bbtool.
sys.path.insert(0, os.path.join(DEST, "scripts", "bbtool"))
from commands.fetch import reconcile
reconcile(dest=DEST, version=VERSION, repo=REPO, local=LOCAL)
```

Wire it in your consumer `platformio.ini`:
```ini
extra_scripts = pre:scripts/fetch_breadboard.py
```

**Split rationale.** The cold-start bootstrap (first two `if/elif` lines) is
irreducible: it must run before `.breadboard/scripts/bbtool` is on `sys.path`.
Once `bbtool` is importable, `reconcile()` handles all subsequent cases —
idempotent up-to-date check, stale refetch, and symlink management — without
duplicating logic in the stub.

**Note:** on a cold start the stub fetches breadboard, then `reconcile()` (lacking a stamp
from the cold clone) re-fetches once to write the `.version` stamp — a one-time extra
shallow fetch on first bootstrap, harmless.

---

## `size` command

Measures flash/RAM footprint of a built PlatformIO/ESP-IDF env, device-free
(no flashing) — runs the target toolchain's `size` binary against the env's
`.elf` and parses the accompanying `.map` file for a per-component archive-
member breakdown. Also supports snapshotting/gating that footprint against a
committed baseline (flash, always; heap, when captured from a live device).

```
python3 scripts/bbtool.py size --build-dir .pio/build/<env> [--component NAME ...] [--elf PATH] [--map PATH] [--arch {xtensa,riscv}] [--size-tool PATH] [--root ROOT] [--target NAME] [--update-baseline | --check] [--flash-threshold-pct PCT] [--heap-from-http BASE_URL]
```

| Flag | Default | Meaning |
|------|---------|---------|
| `--build-dir DIR` | (required) | PlatformIO/ESP-IDF build dir containing `firmware.elf` (+ `firmware.map`) |
| `--elf PATH` | `<build-dir>/firmware.elf` | Explicit `.elf` path |
| `--map PATH` | `<build-dir>/firmware.map` | Explicit `.map` path |
| `--component NAME` | none | Component name to report a `.map` size breakdown for (repeatable); omit for a breakdown of every `bb_*` archive found |
| `--arch {xtensa,riscv}` | `xtensa` | Toolchain arch for the `size` binary |
| `--size-tool PATH` | auto-detect | Explicit `size` binary (overrides toolchain auto-detect) |
| `--root ROOT` | cwd | Repository root, for resolving `.baseline/` |
| `--target NAME` | `<build-dir>` basename (e.g. `esp32`) | Baseline target label |
| `--update-baseline` | off | Measure + snapshot the resolved sdkconfig, write `.baseline/bbtool/metrics/<target>.json` (preserves any existing `heap` block); mutually exclusive with `--check` |
| `--check` | off | Measure + compare against the committed baseline; FAIL on bss growth or flash_total growth beyond `--flash-threshold-pct`; mutually exclusive with `--update-baseline` |
| `--flash-threshold-pct PCT` | `2.0` | Allowed `flash_total` growth vs baseline, in percent |
| `--heap-from-http BASE_URL` | none | Fetch live heap stats from `<BASE_URL>/api/diag/heap` (the `bb_diag` component); only applies with `--update-baseline`/`--check` — with neither, it's a warned no-op |

With no `--update-baseline`/`--check`, emits one JSON line: `{"elf",
"build_dir", "text", "data", "bss", "flash_total", "components"}` —
`flash_total` is `text + data` and is the authoritative aggregate;
per-component `.map` sizes overlap (a symbol can be attributed to multiple
sections) and are not additive against `flash_total`.

### Baseline file

`--update-baseline` writes `.baseline/bbtool/metrics/<target>.json`:

```json
{
  "target": "esp32",
  "arch": "xtensa",
  "config": {
    "label": "default",
    "toolchain": "esp-idf",
    "sdkconfig_sha": "<sha256 of the normalized sdkconfig>",
    "snapshot": ".baseline/bbtool/metrics/esp32.sdkconfig"
  },
  "flash": {
    "text": 0, "data": 0, "bss": 0, "flash_total": 0,
    "components": {"bb_core": 0}
  },
  "heap": {"min_free": null, "high_water": null, "regions": null, "source": null}
}
```

Alongside it, a sibling `.baseline/bbtool/metrics/<target>.sdkconfig` file
captures every resolved `CONFIG_*` knob (not just `CONFIG_BB_*`), one per
line, sorted and deduplicated; `# CONFIG_X is not set` is normalized to the
explicit sentinel `CONFIG_X=n` so a knob flip between "set" and "not set" is
a visible line-level diff, not a silent line removal/addition. `config.
sdkconfig_sha` is the SHA-256 of that normalized snapshot text — a config
drift shows up as a `sdkconfig_sha` change even if flash/RAM happen not to
move. Both files are written atomically (temp file + `os.replace`), so a
kill mid-write can't corrupt a committed baseline.

### Flash gate (`--check`)

- `bss` is **shrink-only** — any growth vs the baseline is a FAIL.
- `flash_total` may grow up to `--flash-threshold-pct` (default 2.0%) before
  it's a FAIL; growth strictly greater than the threshold fails, growth
  exactly at the threshold passes.
- Advance the baseline with `--update-baseline` (a ratchet, mirroring the
  `fence` family's baseline files) once a legitimate growth/shrink is
  reviewed and accepted.

### Heap capture + gate (`--heap-from-http`, device-only)

`--heap-from-http <base_url>` GETs `<base_url>/api/diag/heap` (the `bb_diag`
component's per-capability heap endpoint) — this needs a live, reachable
board, so it's a device/fleet-run capability, not a host/CI one.

- With `--update-baseline`: snapshots the response into the target's `heap`
  block (`source: "http"`, `min_free`/`high_water` from `internal.
  minimum_ever_free`, plus a per-capability `regions` map).
- With `--check`: re-fetches and gates **higher-is-better** — current
  watermarks below the committed baseline's are a FAIL. The heap gate is
  inert (skipped, not failed) while the baseline's `heap` block is null —
  i.e. until a `--heap-from-http --update-baseline` run has populated it.
- With neither `--update-baseline` nor `--check` (plain single-shot mode),
  `--heap-from-http` is a no-op: bbtool prints a `bbtool size: warning:
  --heap-from-http only applies with --update-baseline/--check` message to
  stderr and proceeds with the normal single-shot JSON output/exit code.

### `make size-check` / `make size-baseline`

Manual tooling, not part of the default `make check` gate and not yet wired
into CI:

```
make size-check      # gate examples/smoke's esp32 build against its committed baseline
make size-baseline    # update the esp32 baseline from the current build
```

Both need `examples/smoke/.pio/build/esp32` already built (e.g. `pio run -d
examples/smoke -e esp32`).

### `docs` `bbtool:budget` region

Component READMEs that opt in to the `bbtool:budget` marker (see the `docs`
command below) get a generated footprint table sourced from every committed
`.baseline/bbtool/metrics/*.json` that lists the component in its `flash.
components` map — one row per target, rendering `_(no baseline)_` when none
exists. See the marker-keys table below.

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
