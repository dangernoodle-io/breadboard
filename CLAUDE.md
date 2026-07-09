# breadboard

Measurement-driven, ground-up firmware component framework for no-PSRAM-class ESP32 (and beyond), with multiple backends (ESP-IDF, Arduino/CC3000, Arduino R4, host). Heap is the strict, vigilantly-guarded resource; flash is forgiving; components compose and pay heap only for what you add.

## ⚠️ Composition: TWO paths only — codegen + handwire

**"autowire" is DEAD.** Any reference to `autowire` / `bbtool autowire` as a *composition* mechanism, or to component self-registration (`BB_INIT_REGISTER*`, `*_AUTOREGISTER`, `*_AUTO_ATTACH`, force-register `-u` keeps, pub-sink glue) is LEGACY being removed — ignore it as a pattern to follow. NEVER add a new one (enforced shrink-only by `make fence`, `di_legacy` family).

The only two sanctioned composition paths: (1) **codegen** — `bbtool codegen` generates the composition root `bb_app_init()` from `// bbtool:init tier= fn=` header markers (see `examples/floor` `make floor-codegen`); (2) **handwire** — explicit `app_main`/entry calls the component init fns directly (see `examples/floor/main/floor_app.c`).

`bbtool autowire`+`bbtool size` survive ONLY as measurement tooling (flash/heap budgeting) — NOT a composition mechanism.

Any legacy `bb_init` walker / `BB_INIT_REGISTER*` docs found elsewhere (e.g. still-live in `examples/smoke`) are CURRENT-because-still-used, not a pattern to follow — they're slated for demolition (roadmap: DI dissolution); do not extend them.

## Public symbol prefix

All public C symbols use prefix `bb_`.

## Component discovery

Component inventory and per-component docs live in `components/<name>/README.md` (and `components/README.md` index once it exists), not here.

- discover component internals via the component's own README plus the source
- component behavior is NOT documented in CLAUDE.md — don't grep this file for it

### Documentation routing

Route new content by kind; don't default to CLAUDE.md:
- component behavior/API/knobs → the component's own `components/<name>/README.md`
- authoritative symbol reference → the C API reference site (Doxygen+MkDocs, future, B1-348)
- deep knowledge / worked examples / multi-component layering → the wiki
- internal rationale/process/decisions → the wiki
- cross-cutting project conventions / build+test → CLAUDE.md
- Component READMEs are 100% generated (`bbtool docs scaffold`/`gen`) — brief from the header's `@brief`, deps/platform/api/links derived, plus a link to the wiki narrative — never hand-authored, never hand-edit a README. Narrative (when/how-to-use, design rationale) lives in the wiki `components/<name>.md`. Full authoring/sync workflow: wiki [Component-Docs](https://github.com/dangernoodle-io/breadboard/wiki/Component-Docs)
- do NOT add component-specific prose to CLAUDE.md — it goes in the component README
- one home per fact — don't restate content across CLAUDE.md/wiki/README; link the home

## Finding component documentation

**This routing is INTERIM** — until the staging page (see below) empties out.

To find a component's docs:
1. `components/<name>/README.md` — generated; see wiki [Component-Docs](https://github.com/dangernoodle-io/breadboard/wiki/Component-Docs)
2. if absent, the temporary staging page `breadboard.wiki/Component-Notes-Staging.md` — a shrinking holding area for notes not yet converted to a README (ratchet rule: wiki [Component-Docs](https://github.com/dangernoodle-io/breadboard/wiki/Component-Docs))
3. conceptual/architectural topics + narrative (when/how-to-use) live in the wiki

## Build & test

Default gate: `make check` (lint + cppcheck + docs-index-check + docs-check + fence). Python tooling tests: `make test-py`. Host unit tests: `make test`.

**Verify the CI way** — host tests FALSE-PASS under the local default toolchain; run under the CI gcc-16 `-Werror` PATH-shim (B1-642). Also build a real platform target (`make smoke`), which false-passes on native too. Never trust a local green alone.

Full setup + all targets → wiki [Getting-Started](https://github.com/dangernoodle-io/breadboard/wiki/Getting-Started).

## Conventions

Conventions are lint-enforced — the lint is the canonical rule; full detail + rationale in the wiki. Pointers only below; do not restate rules here (avoids CLAUDE.md/wiki drift).

- Portability (platform-header fencing) — `public-header-leak` — [wiki](https://github.com/dangernoodle-io/breadboard/wiki/Conventions#portability)
- Public API surface — `bb-prefix`, `public-header-inline-platform-call`, `no-arduino-string` — [wiki](https://github.com/dangernoodle-io/breadboard/wiki/Conventions#api-conventions)
- Layout — no lint — [wiki](https://github.com/dangernoodle-io/breadboard/wiki/Conventions#layout)
- Header visibility & coupling (REQUIRES vs PRIV_REQUIRES) — `public-requires-unused`, `pragma-once` — [wiki](https://github.com/dangernoodle-io/breadboard/wiki/Conventions#header-visibility-and-component-coupling)
- Embedding assets — no lint — [wiki](https://github.com/dangernoodle-io/breadboard/wiki/Conventions#embedding-assets)
- Portable timing — `raw-esp-timer`, `raw-timestamp-divide` — [wiki](https://github.com/dangernoodle-io/breadboard/wiki/Conventions#portable-timing)
- Timer callbacks — `timer-cb-heavy` — [wiki](https://github.com/dangernoodle-io/breadboard/wiki/Conventions#timer-callback-convention)
- Logging — no lint — [wiki](https://github.com/dangernoodle-io/breadboard/wiki/Conventions#logging)
- Audit-class defect ratchets (Kconfig bridge, reuse/idiom, branch coverage, route-init purity) — `kconfig-bridge-shadow`, plus non-lint items — [wiki](https://github.com/dangernoodle-io/breadboard/wiki/Conventions#audit-class-regressions)
- Composition-only — codegen + handwire are the only sanctioned paths, the legacy DI/registry surface (`BB_INIT_REGISTER`, `*_AUTOREGISTER`, force-register keeps) is frozen shrink-only (see the banner at top); never add new uses (enforced by `make fence`, `di-legacy` family — `di-fence` remains as a back-compat alias); `--update-baseline` only prunes, a genuine conversion needs a reviewed baseline edit (see `scripts/bbtool/README.md`) — [wiki](https://github.com/dangernoodle-io/breadboard/wiki/design/DI-Model)
- Consolidation — new shared/numeric/string/parse helpers land in their central component (`bb_core`/`bb_num`/`bb_str`/`bb_scalar`) from the get-go, never re-hand-rolled; the `fence` ratchets enforce it — [wiki](https://github.com/dangernoodle-io/breadboard/wiki/Conventions#reuse-or-extract-shared-helpers-dont-re-hand-roll-an-idiom)
- Component creation — extend by default; a NEW `components/<name>/` requires a distinct dependency + a real consumer + design sign-off (no speculative/ad-hoc components); enforced by `make fence`, `new_component` family — a new component fails the fence until approved via `fence --approve <name>`, the one baseline that legitimately grows (see `scripts/bbtool/README.md`) — [wiki](https://github.com/dangernoodle-io/breadboard/wiki/design/Component-Taxonomy#when-to-create-a-new-component)
- Backend dispatch — vtable (runtime-registered) vs flat per-platform-TU (link-time); pick one per component — [wiki](https://github.com/dangernoodle-io/breadboard/wiki/design/Backend-Dispatch)

## Optional tooling (if installed)

This project builds and tests with plain ESP-IDF/PlatformIO and assumes no extra tools. The following is an optional aid, not a requirement:

- **pogopin** (MCP) — on-device hardware ops: flash, serial monitor, panic-backtrace decode; use it for the build -> flash -> verify loop. Optional; not required.

## Releases

Tagging is manual: `git tag -a vX.Y.Z -m 'chore: vX.Y.Z tag' && git push origin vX.Y.Z`. The `release.yml` workflow waits for CI then publishes a GitHub release with auto-generated notes categorized by PR label (`.github/release.yml`). PR labels are auto-applied from conventional-commit prefixes; `new-component` PRs need that label set manually.
