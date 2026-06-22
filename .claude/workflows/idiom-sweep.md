# Idiom-duplication sweep (pre-release hygiene)

Whole-repo, **not** diff-scoped — catches copy-pasted code shapes that a per-PR
review misses (the class that shipped the SPIRAM-alloc, HTTP-status, query-scan,
section-assemble, and HAL-snapshot duplication found in the v0.62.0→main audit).

Run it before cutting a release, or after a burst of feature work.

## How to run

Pin all reads to a stable ref so a concurrent merge can't shift them:

```
SHA=$(git rev-parse origin/main)
```

Launch **4 parallel finder subagents** (sonnet), one per idiom domain. Each reads
ONLY via `git grep <pat> $SHA -- components platform` and `git show $SHA:<file>`
(never the working tree), and returns a ranked list: for every code shape
appearing in ≥2 sites — the sites (file:line), copy-count, whether a shared
helper already exists, the proposed helper + its home, effort (S/M/L), and risk.

Domains:
1. **Allocation/memory** — SPIRAM-preferred alloc + fallback, `heap_caps_*`
   wrappers vs the existing `bb_board_heap_*` / `bb_mem` helpers, fixed scratch
   buffers, alloc/free unwind ladders.
2. **Concurrency** — lock+check-dead+capture+unlock (the `bb_mqtt_publish`
   guard), poll→cache / snapshot→read HAL pattern, `portMUX` critical sections,
   nullable-mutex take/give, lazy-init-under-lock singletons.
3. **HTTP / serialization** — query-string parsing, status-code→reason mapping,
   JSON gather→emit / section freeze+assemble boilerplate, SSE task scaffold,
   recv-body malloc/parse/free.
4. **Config / peripheral** — the inert-knob anti-pattern (`#ifndef BB_X` shadowing
   `CONFIG_BB_X` — grep every component), PMBus/I2C primitive duplication vs
   `bb_i2c_dev_*`, decode-helper structure, Kconfig default-vs-literal drift.

Then **adversarially verify** any P0/P1 finding (a second agent that tries to
refute it) before acting — datasheet/bit-level or behavior claims especially.

Synthesize into ranked backlog items. Fixes follow the serial pipeline
(1 PR + 1 agent in flight; see workspace `feedback_serialized_pr_pipeline`).

## Notes
- The exact finder prompts used for the v0.62.0→main run are in the session
  history; re-derive per the domains above.
- Pair with the B1-263 CI lint (header/type-leak + REQUIRES hygiene) — that's the
  push-time enforcement; this sweep is the periodic deep pass.
