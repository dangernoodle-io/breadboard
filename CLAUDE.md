# breadboard

Measurement-driven, ground-up firmware component framework for no-PSRAM-class ESP32 (and beyond), with multiple backends (ESP-IDF, Arduino/CC3000, Arduino R4, host). Heap is the strict, vigilantly-guarded resource; flash is forgiving; components compose and pay heap only for what you add.

## ⚠️ Composition: TWO paths only — codegen + handwire

**"autowire" is DEAD.** Any reference to `autowire` / `bbtool autowire` as a *composition* mechanism, or to component self-registration (`BB_INIT_REGISTER*`, `*_AUTOREGISTER`, `*_AUTO_ATTACH`, force-register `-u` keeps, pub-sink glue) is LEGACY being removed — ignore it as a pattern to follow. NEVER add a new one (enforced shrink-only by `make fence`, `di_legacy` family).

The only two sanctioned composition paths: (1) **codegen** — `bbtool codegen` generates the composition root `bb_app_init()` from `// bbtool:init tier= fn=` header markers (see `examples/floor` `make floor-codegen`); (2) **handwire** — explicit `app_main`/entry calls the component init fns directly (see `examples/floor/main/floor_app.c`).

The `bbtool autowire` CLI command has been deleted (its transitive-closure resolver survives as library code in `scripts/bbtool/composition.py`, used only by `bbtool codegen`). `bbtool size` survives as measurement tooling (flash/heap budgeting) — NOT a composition mechanism.

The `bb_init` walker component (the runtime registry that used to drive `BB_INIT_REGISTER*`) has been deleted — nothing composes through it anymore. Any doc or comment still referencing it is historical/contrastive, not a live pattern; do not resurrect it.

breadboard ships **primitives** (seams, topic APIs, helpers), never glue components — wiring is the consumer's job, via codegen or handwire. Full positive-model narrative + two-firmware validation (`examples/floor` handwire, `examples/smoke` codegen): wiki [DI-Model](https://github.com/dangernoodle-io/breadboard/wiki/design/DI-Model).

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

**Verify the CI way** — two independent local-vs-CI traps, both closed the same way (a genuine GNU toolchain, matched `gcc`/`gcov` major version): (1) host tests FALSE-PASS under macOS's default toolchain (Apple clang masquerading as `gcc`/`gcov`) — a LOCAL requirement, not a reproduction of CI (CI has never pinned a specific gcc version; `pio test` there runs under ubuntu-latest's stock gcc with no shim or version check at all); (2) `-Werror` is set repo-wide but `-Wall`/`-Wformat` are not, and Ubuntu's gcc patches `-Wformat` on by default while Homebrew's does not — a genuine GNU toolchain that ships a DIFFERENT default warning set can still local-clean/CI-fail (PR #1218: `-Werror=format-truncation` only in CI). The `warning_fence_host`/`warning_fence_firmware` families (`scripts/bbtool/fence/_gcc_warnings.py`, B1-1420) freeze the known `-Wall` backlog shrink-only against a LIVE build artifact, not a committed snapshot: `.github/workflows/build.yml`'s `test` job (host) and `smoke` job's `esp32` leg (firmware) each run `scripts/warn_scan.sh <target>` then fence-check its fresh output in the same job, with `BB_WARNING_FENCE_STRICT=1` so a missing/empty/unreadable/build-failed report hard-fails rather than silently passing; outside those two jobs (e.g. the toolchain-less `check` job, or a dev machine) the same check is a loud SKIP, never a false pass or a false fail. Also build a real platform target (`make smoke`), which false-passes on native too. Never trust a local green alone.

**Test verdict** — `make test` never trusts `pio test`'s own pass/fail verdict: PlatformIO's native test reader can misreport a genuine failure (B1-1137), so `pio test` is used to build only, and `scripts/run_host_tests.py` derives the verdict from a direct run of the compiled binary instead — and `pio test`'s own exit code must also be clean, so a compile failure can never be masked by a stale, previously-passing binary.

**Host test log: read all summaries** — the host-test log spans ~5500 lines and emits summary blocks for each of four native environments (native, native_lock_stats_off, native_openapi_runtime_meta, native_prov_gate_disable); the tail shows only the last. Read all four summary blocks and report per-environment counts; never report the tail alone.

**Coverage gate** — `make test`/`make coverage` self-shim via `scripts/coverage_toolchain.sh`, which resolves + PATH-shims a matched, genuinely-GNU `gcc`/`g++`/`gcov` triple (any major version — no specific number is required; `COVERAGE_GCC_MAJOR` opt-in-pins one if ever needed), aborting loudly rather than silently using the wrong toolchain (escape hatch `COVERAGE_ALLOW_ANY_TOOLCHAIN=1`, not recommended). Two independent traps this closes: (1) B1-642 — PlatformIO strips `CC`/`CXX`, so only a PATH shim actually works; (2) B1-867 — Apple's LLVM `gcov` can't read GNU `.gcno`/`.gcda` and silently reports a zeroed/bogus result instead of erroring; the shim also rejects any gcc/gcov major-version mismatch, which produces the same kind of garbage output. `make coverage` also gates on BOTH line and branch coverage (Coveralls, the real merge gate, gates on lines — a prior branch-only local gate missed a line-only regression, PR #845) and aborts if either metric comes back exactly 0% (a tooling failure, never a real result over this repo). The local gate's 100%/100% floor is a deliberate house rule stricter than Coveralls's non-regression gate — see `scripts/coverage_gate.py`.

**Coverage gate: local vs CI differ on purpose (B1-1258)** — the per-file ratchet gates both lines and branches locally, but in CI it ratchets LINES ONLY, because branch attribution varies by gcc major and the branch baseline is seeded on a dev machine; Coveralls is the CI authority for branch coverage. See `scripts/coverage_baseline.py` and `scripts/coverage_gate.py`'s `COVERAGE_GATE_LINES_ONLY`.

Full setup + all targets → wiki [Getting-Started](https://github.com/dangernoodle-io/breadboard/wiki/Getting-Started).

## Conventions

Conventions are lint-enforced — the lint is the canonical rule; full detail + rationale in the wiki. Pointers only below; do not restate rules here (avoids CLAUDE.md/wiki drift).

- Portability (platform-header fencing) — `public-header-leak` — [wiki](https://github.com/dangernoodle-io/breadboard/wiki/Conventions#portability)
- Public API surface — `bb-prefix`, `public-header-inline-platform-call`, `no-arduino-string` — [wiki](https://github.com/dangernoodle-io/breadboard/wiki/Conventions#api-conventions)
- No variant ladders — one entry point per public API, extend via config-struct fields; no lint — [wiki](https://github.com/dangernoodle-io/breadboard/wiki/Conventions#api-conventions)
- `bb_data` binding kind: EVENT vs STATE — a binding may only be EVENT-kind if its info isn't reconstructible from STATE; most event-shaped things fold into a counter + latest-value field; no lint — [wiki](https://github.com/dangernoodle-io/breadboard/wiki/Conventions#bb_data-binding-kind-event-vs-state-reconstructibility-rule)
- Layout — no lint — [wiki](https://github.com/dangernoodle-io/breadboard/wiki/Conventions#layout)
- Header visibility & coupling (REQUIRES vs PRIV_REQUIRES) — `public-requires-unused`, `pragma-once` — [wiki](https://github.com/dangernoodle-io/breadboard/wiki/Conventions#header-visibility-and-component-coupling)
- Embedding assets — no lint — [wiki](https://github.com/dangernoodle-io/breadboard/wiki/Conventions#embedding-assets)
- Portable timing — `raw-esp-timer`, `raw-timestamp-divide` — [wiki](https://github.com/dangernoodle-io/breadboard/wiki/Conventions#portable-timing)
- Timer callbacks — `timer-cb-heavy` — [wiki](https://github.com/dangernoodle-io/breadboard/wiki/Conventions#timer-callback-convention)
- Logging — no lint — [wiki](https://github.com/dangernoodle-io/breadboard/wiki/Conventions#logging)
- Audit-class defect ratchets (Kconfig bridge, reuse/idiom, branch coverage, route-init purity) — `kconfig-bridge-shadow`, plus non-lint items — [wiki](https://github.com/dangernoodle-io/breadboard/wiki/Conventions#audit-class-regressions)
- Composition-only — codegen + handwire are the only sanctioned paths, the legacy DI/registry surface (`BB_INIT_REGISTER`, `*_AUTOREGISTER`, force-register keeps) is frozen shrink-only (see the banner at top); never add new uses (enforced by `make fence`, `di-legacy` family — `di-fence` remains as a back-compat alias); `--update-baseline` only prunes, a genuine conversion needs a reviewed baseline edit (see `scripts/bbtool/README.md`) — [wiki](https://github.com/dangernoodle-io/breadboard/wiki/design/DI-Model)
- Consolidation — new shared/numeric/string/parse helpers land in their central component (`bb_core`/`bb_num`/`bb_str`/`bb_scalar`) from the get-go, never re-hand-rolled; the SECOND hand-rolled instance of a shared idiom (anywhere, including same-file) triggers extraction — no waiting for a third, no same-file exception; the `fence` ratchets enforce it — [wiki](https://github.com/dangernoodle-io/breadboard/wiki/Conventions#reuse-or-extract-shared-helpers-dont-re-hand-roll-an-idiom)
- Component creation — extend by default; a NEW `components/<name>/` requires a distinct dependency + a real consumer + design sign-off (no speculative/ad-hoc components); enforced by `make fence`, `new_component` family — a new component fails the fence until approved via `fence --approve <name>`, the one baseline that legitimately grows (see `scripts/bbtool/README.md`) — [wiki](https://github.com/dangernoodle-io/breadboard/wiki/design/Component-Taxonomy#when-to-create-a-new-component)
- Backend dispatch — vtable (runtime-registered) vs flat per-platform-TU (link-time); pick one per component — [wiki](https://github.com/dangernoodle-io/breadboard/wiki/design/Backend-Dispatch)

## Optional tooling (if installed)

This project builds and tests with plain ESP-IDF/PlatformIO and assumes no extra tools. The following is an optional aid, not a requirement:

- **pogopin** (MCP) — on-device hardware ops: flash, serial monitor, panic-backtrace decode; use it for the build -> flash -> verify loop. Optional; not required.

## Releases

Tagging is manual: `git tag -a vX.Y.Z -m 'chore: vX.Y.Z tag' && git push origin vX.Y.Z`. The `release.yml` workflow waits for CI then publishes a GitHub release with auto-generated notes categorized by PR label (`.github/release.yml`). PR labels are auto-applied from conventional-commit prefixes; `new-component` PRs need that label set manually.
