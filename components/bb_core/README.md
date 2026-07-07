# bb_core

Foundational, near-zero-dep primitives every other `bb_*` component builds on: the portable error type, the canonical clock, run-exactly-once, a contention-instrumented lock, byte-order helpers, memory accounting, and the reboot-reason codec.

## When to use / when not

Every component `REQUIRES bb_core` for `bb_err_t`/`BB_OK`/`BB_ERR_*` and the opaque HTTP handle types. Reach for `bb_clock_now_us()`/`bb_clock_now_ms64()` instead of hand-rolling `esp_timer_get_time()`/`clock_gettime()`; `bb_once_run()` instead of a hand-rolled double-checked-init flag; `bb_lock_*` instead of a bare `pthread_mutex_t` when contention/wait/hold-time visibility is wanted. Don't reach for `bb_lock` when you don't need the instrumentation — a bare `pthread_mutex_t` (or `BB_LOCKED_COPY`) is lighter when stats are never queried.

## Public API

See [`include/bb_core.h`](include/bb_core.h), [`include/bb_clock.h`](include/bb_clock.h), [`include/bb_once.h`](include/bb_once.h), [`include/bb_lock.h`](include/bb_lock.h), [`include/bb_mem.h`](include/bb_mem.h), [`include/bb_byte_order.h`](include/bb_byte_order.h), [`include/bb_claim.h`](include/bb_claim.h), [`include/bb_reboot_reason.h`](include/bb_reboot_reason.h). Key symbols:

- `bb_err_t`, `BB_OK` / `BB_ERR_*` — portable error type (`bb_core.h`).
- `bb_clock_now_us()` / `bb_clock_now_ms()` / `bb_clock_now_ms64()` — canonical monotonic clock (`bb_clock.h`); never hand-roll `esp_timer_get_time()/1000`.
- `bb_once_t`, `BB_ONCE_INIT`, `bb_once_run(once, fn, ctx)` — run `fn` exactly once across any number of concurrent callers; every other caller blocks (yielding) until that run completes (`bb_once.h`).
- `bb_lock_t`, `bb_lock_config_t`, `bb_lock_init/destroy/lock/trylock/unlock`, `bb_lock_get_stats/reset_stats`, `bb_lock_stats_set_enabled/stats_enabled` — opaque, contention-instrumented mutex; stats are a two-level gate (compile `BB_LOCK_STATS_ENABLE` + runtime flag), zero-cost when off (`bb_lock.h`).
- `BB_LOCKED_COPY(mtx_ptr, dst, src)` — lock/copy/unlock helper for single-assignment critical sections (`bb_lock.h`).

Public symbols in this component use the `bb_` prefix.

## Config knobs

| Kconfig | Default | Notes |
|---------|---------|-------|
| `CONFIG_BB_LOCK_STATS_ENABLE` | n | Compile gate for `bb_lock` contention/wait/hold-time instrumentation. Off = plain mutex lock/unlock, zero added cost. See `components/bb_core/Kconfig`. |
| `CONFIG_BB_MEM_STATS_ENABLE` | n | Compile gate for `bb_mem` allocation accounting (outstanding/peak bytes, alloc/free/fail counts). |
| `BB_OTA_STRATEGY` | `BB_OTA_STRATEGY_PULL` | OTA update strategy choice (`/api/update/apply` owner) — see the OTA strategy section in the repo `CLAUDE.md`. |

## Dependencies

<!-- BEGIN bbtool:deps -->
**REQUIRES:** _(none)_

**PRIV_REQUIRES:** `esp_timer`, `freertos`
<!-- END bbtool:deps -->

## Platform support

<!-- BEGIN bbtool:platform -->
| host | espidf | arduino |
|------|--------|---------|
| yes | yes | no |
<!-- END bbtool:platform -->

## Links

[![build](https://github.com/dangernoodle-io/breadboard/actions/workflows/build.yml/badge.svg)](https://github.com/dangernoodle-io/breadboard) [![coverage](https://coveralls.io/repos/github/dangernoodle-io/breadboard/badge.svg?branch=main)](https://github.com/dangernoodle-io/breadboard)

- Repository: [https://github.com/dangernoodle-io/breadboard](https://github.com/dangernoodle-io/breadboard)
- Wiki: [https://github.com/dangernoodle-io/breadboard/wiki](https://github.com/dangernoodle-io/breadboard/wiki)

## See also

`bb_task` (task creation/registry), `bb_timer` (esp_timer/bb_timer deferred + worker callback idioms), `bb_diag`/`bb_system` (reboot-reason SSOT built on `bb_reboot_reason.h`'s codec).
