// bb_transport_health — per-transport connectivity-health SSOT.
//
// Tracks the up/down state of every registered transport (MQTT, HTTP sink,
// stratum socket, ...) in one place, split into two classes:
//
//   AUTHORITATIVE — the transport itself reports success/failure explicitly
//     via bb_transport_health_report(). Only AUTHORITATIVE slots contribute
//     to bb_transport_health_authoritative_counts() — this is the
//     observe-only guarantee: an INFERRED transport can never trip a
//     downstream "N transports failing" decision.
//
//   INFERRED — health is inferred from received-traffic recency via
//     bb_transport_health_mark_activity(); staleness is computed at
//     snapshot time (bb_transport_health_is_stale), never stored.
//
// Pure, host-testable core with a single global lock guarding every slot.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "bb_core.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Kconfig bridge — pattern from bb_clock.h / CLAUDE.md. Never shadow the
// generated CONFIG_ symbol with a bare #ifndef.
// ---------------------------------------------------------------------------

#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#ifdef CONFIG_BB_TRANSPORT_HEALTH_MAX_SLOTS
#define BB_TRANSPORT_HEALTH_MAX_SLOTS CONFIG_BB_TRANSPORT_HEALTH_MAX_SLOTS
#endif
#ifdef CONFIG_BB_TRANSPORT_HEALTH_INFERRED_STALE_S
#define BB_TRANSPORT_HEALTH_INFERRED_STALE_S CONFIG_BB_TRANSPORT_HEALTH_INFERRED_STALE_S
#endif
#endif
#ifndef BB_TRANSPORT_HEALTH_MAX_SLOTS
#define BB_TRANSPORT_HEALTH_MAX_SLOTS 8
#endif
#ifndef BB_TRANSPORT_HEALTH_INFERRED_STALE_S
#define BB_TRANSPORT_HEALTH_INFERRED_STALE_S 60
#endif

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

typedef int bb_transport_handle_t;
#define BB_TRANSPORT_HANDLE_INVALID (-1)

typedef enum {
    BB_TRANSPORT_AUTHORITATIVE = 0,
    BB_TRANSPORT_INFERRED      = 1,
} bb_transport_class_t;

typedef struct {
    const char           *name;
    bb_transport_class_t  cls;
    bool                  enabled;
    bool                  failing;
    uint64_t              last_ok_ms;
    uint32_t              fail_count;
    uint64_t              last_rx_ms;
    uint32_t              rx_count;
} bb_transport_health_snapshot_t;

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------

// Registers a transport slot. `name` is stored by pointer — caller must
// supply a string with static lifetime (e.g. a string literal). Returns
// BB_ERR_NO_SPACE when BB_TRANSPORT_HEALTH_MAX_SLOTS is exhausted.
bb_err_t bb_transport_health_register(const char *name, bb_transport_class_t cls,
                                       bb_transport_handle_t *out);

// Enables/disables a slot. Disabled slots are excluded from
// bb_transport_health_authoritative_counts().
bb_err_t bb_transport_health_set_enabled(bb_transport_handle_t h, bool enabled);

// AUTHORITATIVE only. ok=true clears failing + stamps last_ok_ms; ok=false
// sets failing + bumps fail_count. Returns BB_ERR_INVALID_ARG if h is an
// INFERRED slot.
bb_err_t bb_transport_health_report(bb_transport_handle_t h, bool ok);

// INFERRED only. Stamps last_rx_ms + bumps rx_count. Returns
// BB_ERR_INVALID_ARG if h is an AUTHORITATIVE slot.
bb_err_t bb_transport_health_mark_activity(bb_transport_handle_t h);

// Counts ONLY enabled AUTHORITATIVE slots. INFERRED slots never contribute —
// this is the observe-only guarantee: an INFERRED transport, however stale,
// cannot influence this count.
bb_err_t bb_transport_health_authoritative_counts(int *out_enabled, int *out_failing);

// Copies up to `max` slot snapshots into `out`. For INFERRED slots,
// `failing` is computed at call time via bb_transport_health_is_stale()
// against the current clock. Single lock scope over the whole copy.
// Returns the number of entries written.
size_t bb_transport_health_snapshot_all(bb_transport_health_snapshot_t *out, size_t max);

// Pure predicate: true when (now_ms - last_rx_ms) exceeds threshold_s
// seconds. No internal clock read — explicit args, host-testable.
bool bb_transport_health_is_stale(uint64_t last_rx_ms, uint64_t now_ms, uint32_t threshold_s);

#ifdef BB_TRANSPORT_HEALTH_TESTING
void bb_transport_health_reset_for_test(void);
#endif

#ifdef __cplusplus
}
#endif
