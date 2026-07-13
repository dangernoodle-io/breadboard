// bb_system reboot budget (B1-863) — shared cooldown + daily-cap rate limit
// for reboot-as-recovery-action consumers, keyed per bb_reboot_cause_t.
//
// Portable: compiled on host AND ESP-IDF, no ESP_PLATFORM gate beyond the
// bb_storage backend-name pick just below. The epoch-resolving public
// wrapper (bb_system_reboot_budget_allows/record) is DELIBERATELY per-
// platform instead (platform/espidf/bb_system/bb_system.c resolves the
// real bb_ntp_is_synced()/time(NULL); platform/host/bb_system/
// bb_system_host.c is a straight-line call with synced=false) — host has
// no NTP and bb_ntp_is_synced() is hardcoded false there with no seam to
// force it true, so putting the synced/unsynced branch in this shared file
// would have left that branch permanently uncovered on host (not a
// legitimate LCOV_EXCL — the line isn't UB-guarded, it's just unreachable
// given today's host stub). Splitting the epoch resolution out per-
// platform keeps bb_system_reboot_budget_allows_at/record_at (where the
// actual decision logic lives) fully host-tested via explicit args.
#include "bb_system.h"
#include "bb_storage.h"

#ifdef BB_SYSTEM_TESTING
#include "bb_system_test.h"
#endif

#include <string.h>
#include <stdio.h>
#include <inttypes.h>

// bb_storage backend + NVS namespace this file persists under.
//
// Backend: "nvs" on device (bb_storage_nvs), "ram" on host — bb_storage_ram
// is the EXISTING in-memory backend (registered by name at runtime in host
// tests), not a new fake.
#ifdef ESP_PLATFORM
#define BB_SYSTEM_REBOOT_BUDGET_BACKEND "nvs"
#else
#define BB_SYSTEM_REBOOT_BUDGET_BACKEND "ram"
#endif

// Namespace: duplicated from bb_nv_namespaces.h's BB_REBOOT_NVS_NS
// ("bb_reboot", co-located with the existing boot-health bookkeeping)
// rather than including that header — keeps this portable file free of any
// dependency on bb_nv, which is dissolving (B1-708).
#define BB_SYSTEM_REBOOT_BUDGET_NVS_NS "bb_reboot"

static const char *cause_key(bb_reboot_cause_t cause)
{
    switch (cause) {
    case BB_REBOOT_CAUSE_WIFI_SAFEGUARD: return "budget_wifi";
    case BB_REBOOT_CAUSE_EGRESS_TIER3:   return "budget_egress";
    default:                             return NULL;
    }
}

uint32_t bb_system_elapsed_epoch_s(uint32_t now_s, uint32_t since_s)
{
    return (now_s >= since_s) ? (now_s - since_s) : 0U;
}

bool bb_system_reboot_budget_should_allow(uint32_t                                now_s,
                                           uint32_t                                min_interval_s,
                                           uint32_t                                daily_cap,
                                           const bb_system_reboot_budget_state_t *st)
{
    if (!st) return false;

    if (st->last_reboot_s != 0U &&
        bb_system_elapsed_epoch_s(now_s, st->last_reboot_s) < min_interval_s) {
        return false; // cooldown not elapsed
    }

    uint8_t n = st->ring_count;
    if (n > BB_SYSTEM_REBOOT_BUDGET_CAP_MAX) n = BB_SYSTEM_REBOOT_BUDGET_CAP_MAX;
    uint32_t count24h = 0;
    for (uint8_t i = 0; i < n; i++) {
        uint32_t ts = st->reboot_s_ring[i];
        if (now_s < ts) continue; // clock skew: exclude from the window
        if ((now_s - ts) < 86400U) count24h++;
    }
    if (count24h >= daily_cap) {
        return false; // daily cap exhausted
    }

    return true;
}

void bb_system_reboot_budget_state_record(bb_system_reboot_budget_state_t *st, uint32_t now_s)
{
    if (!st) return;

    st->reboot_s_ring[st->ring_head] = now_s;
    st->ring_head = (uint8_t)((st->ring_head + 1U) % BB_SYSTEM_REBOOT_BUDGET_CAP_MAX);
    if (st->ring_count < BB_SYSTEM_REBOOT_BUDGET_CAP_MAX) {
        st->ring_count++;
    }
    st->last_reboot_s = now_s;
}

bool bb_system_reboot_budget_state_encode(const bb_system_reboot_budget_state_t *st,
                                           char *buf, size_t buf_len)
{
    if (!st || !buf || buf_len == 0U) return false;

    int off = snprintf(buf, buf_len, "%" PRIu32 "|%u|%u",
                        st->last_reboot_s,
                        (unsigned)st->ring_head,
                        (unsigned)st->ring_count);
    if (off < 0) return false;  // LCOV_EXCL_BR_LINE — snprintf of a numeric-only format never returns <0
    if ((size_t)off >= buf_len) return false;

    for (uint8_t i = 0; i < BB_SYSTEM_REBOOT_BUDGET_CAP_MAX; i++) {
        int n = snprintf(buf + off, buf_len - (size_t)off, "%s%" PRIu32,
                          (i == 0) ? "|" : ",", st->reboot_s_ring[i]);
        if (n < 0) return false;  // LCOV_EXCL_BR_LINE — snprintf of a numeric-only format never returns <0
        if ((size_t)n >= buf_len - (size_t)off) return false;
        off += n;
    }

    return true;
}

bool bb_system_reboot_budget_state_decode(const char *str, bb_system_reboot_budget_state_t *out)
{
    if (!str || !out) return false;

    bb_system_reboot_budget_state_t decoded;
    memset(&decoded, 0, sizeof(decoded));

    unsigned last_reboot_field = 0, ring_head_field = 0, ring_count_field = 0;
    int consumed = 0;
    // Note: last_reboot_s is parsed as unsigned into a wider temp because
    // scanf's %u has no direct uint32_t specifier portable across libc; the
    // valid epoch-seconds range fits comfortably in unsigned on every target
    // this project builds for.
    if (sscanf(str, "%u|%u|%u|%n", &last_reboot_field, &ring_head_field,
               &ring_count_field, &consumed) != 3) {
        return false;
    }
    if (consumed == 0) return false;  // LCOV_EXCL_BR_LINE — %n after a 3-field match always advances
    if (ring_head_field >= BB_SYSTEM_REBOOT_BUDGET_CAP_MAX ||
        ring_count_field > BB_SYSTEM_REBOOT_BUDGET_CAP_MAX) {
        return false;
    }
    decoded.last_reboot_s = (uint32_t)last_reboot_field;
    decoded.ring_head     = (uint8_t)ring_head_field;
    decoded.ring_count    = (uint8_t)ring_count_field;

    const char *p = str + consumed;
    for (uint8_t i = 0; i < BB_SYSTEM_REBOOT_BUDGET_CAP_MAX; i++) {
        unsigned ts_field = 0;
        int n = 0;
        if (sscanf(p, "%u%n", &ts_field, &n) != 1) {
            return false;
        }
        if (n == 0) return false;  // LCOV_EXCL_BR_LINE — %n after a %u match always advances
        decoded.reboot_s_ring[i] = (uint32_t)ts_field;
        p += n;
        if (i < (uint8_t)(BB_SYSTEM_REBOOT_BUDGET_CAP_MAX - 1)) {
            if (*p != ',') return false;
            p += 1;
        }
    }

    *out = decoded;
    return true;
}

// Load a cause's persisted state. *out is always written: a missing key, a
// backend error, or a corrupt/malformed stored value all decode-fail
// safely into a zero-init state ("no reboots recorded yet") rather than
// propagating an error — matches the precedent this replaces (bb_net_health's
// old egress_act_load_state).
static void reboot_budget_load(bb_reboot_cause_t cause, bb_system_reboot_budget_state_t *out)
{
    memset(out, 0, sizeof(*out));

    const char *key = cause_key(cause);
    if (!key) return;

    bb_storage_addr_t addr = { .backend = BB_SYSTEM_REBOOT_BUDGET_BACKEND,
                                .ns_or_dir = BB_SYSTEM_REBOOT_BUDGET_NVS_NS, .key = key };
    char buf[BB_SYSTEM_REBOOT_BUDGET_STATE_STR_MAX];
    size_t out_len = 0;
    bb_err_t err = bb_storage_get(&addr, buf, sizeof(buf), &out_len);
    if (err != BB_OK || out_len == 0 || out_len > sizeof(buf)) {
        return; // no persisted state (missing key / backend error / oversized) -> zero-init fallback
    }
    buf[sizeof(buf) - 1] = '\0'; // defensive explicit termination (the stored value already includes its own NUL)

    if (!bb_system_reboot_budget_state_decode(buf, out)) {
        memset(out, 0, sizeof(*out)); // corrupt/malformed -> reset to zero-init
    }
}

// Persist a cause's state. Failures are logged nowhere and not propagated
// (matches the precedent this replaces, egress_act_persist_state) — a
// failed persist just means the next boot re-derives a fresh budget, never
// a crash or a blocked reboot decision.
static void reboot_budget_save(bb_reboot_cause_t cause, const bb_system_reboot_budget_state_t *st)
{
    const char *key = cause_key(cause);
    if (!key) return;

    char buf[BB_SYSTEM_REBOOT_BUDGET_STATE_STR_MAX];
    // buf is sized to BB_SYSTEM_REBOOT_BUDGET_STATE_STR_MAX, the documented
    // worst-case for ANY bb_system_reboot_budget_state_t (every field is a
    // bounded-width uint32/uint8 regardless of value: the worst case is
    // ~126-129 bytes against a 192-byte buffer, >=63 bytes of margin) --
    // encode() cannot fail here without a buffer smaller than that, which
    // this function never passes. Not gate-relevant: since PR #850 the
    // coverage gate is lines-only (branch coverage is measured/reported but
    // never per-marker ratcheted), so this branch needs no LCOV_EXCL marker.
    if (!bb_system_reboot_budget_state_encode(st, buf, sizeof(buf))) return;

    bb_storage_addr_t addr = { .backend = BB_SYSTEM_REBOOT_BUDGET_BACKEND,
                                .ns_or_dir = BB_SYSTEM_REBOOT_BUDGET_NVS_NS, .key = key };
    // Store the NUL terminator too so a full-length bb_storage_get() above
    // hands back an already-terminated buffer.
    bb_storage_set(&addr, buf, strlen(buf) + 1);
}

// ---------------------------------------------------------------------------
// Per-cause cache — lazy-loaded once, write-through thereafter, so a
// SUSTAINED unhealthy window (the evaluator re-checking _allows_at() every
// tick while synced) costs ONE storage read total, not one per tick. This
// restores the access pattern of the code this file replaces
// (bb_net_health's old s_reboot_state, loaded once at bb_net_health_start()
// and touched again only on an actual reboot) — bb_storage/bb_config are
// deliberately cacheless BY DESIGN for config (mutable via HTTP PATCH, a
// cache would go stale), but the reboot budget is STATE, not config: one
// writer, one reader, both inside this file, nobody to go stale against.
//
// SAFE WITHOUT A LOCK ONLY because each cause has exactly ONE writer task —
// this is a LOAD-BEARING INVARIANT, not an accident:
//   BB_REBOOT_CAUSE_WIFI_SAFEGUARD  <- the wifi_reconn manager task (once armed)
//   BB_REBOOT_CAUSE_EGRESS_TIER3    <- the bb_net_health evaluator task
// If a future consumer ever needs a second writer for the SAME cause, this
// cache needs a lock first — do not add one without revisiting this.
static bb_system_reboot_budget_state_t s_cache[BB_REBOOT_CAUSE_COUNT];
static bool                            s_loaded[BB_REBOOT_CAUSE_COUNT];

static bool cause_valid(bb_reboot_cause_t cause)
{
    return cause_key(cause) != NULL;
}

// Returns the cache slot for cause, loading it from storage on first access
// only. Returns NULL for an out-of-range cause (nothing to cache).
static bb_system_reboot_budget_state_t *cache_get(bb_reboot_cause_t cause)
{
    if (!cause_valid(cause)) return NULL;
    if (!s_loaded[cause]) {
        reboot_budget_load(cause, &s_cache[cause]);
        s_loaded[cause] = true;
    }
    return &s_cache[cause];
}

#ifdef BB_SYSTEM_TESTING
void bb_system_reboot_budget_reset_for_test(void)
{
    memset(s_cache, 0, sizeof(s_cache));
    memset(s_loaded, 0, sizeof(s_loaded));
}
#endif

bool bb_system_reboot_budget_allows_at(bb_reboot_cause_t cause, bool synced, uint32_t now_s)
{
    if (!synced) {
        return true; // never-synced board: safe direction, no storage I/O -- not even a lazy cache load
    }

    bb_system_reboot_budget_state_t zero;
    memset(&zero, 0, sizeof(zero));
    bb_system_reboot_budget_state_t *st = cache_get(cause);
    if (!st) st = &zero; // out-of-range cause -> treat as a fresh, always-allowed state

    return bb_system_reboot_budget_should_allow(now_s,
                                                 (uint32_t)BB_SYSTEM_REBOOT_BUDGET_MIN_INTERVAL_S,
                                                 (uint32_t)BB_SYSTEM_REBOOT_BUDGET_DAILY_CAP,
                                                 st);
}

void bb_system_reboot_budget_record_at(bb_reboot_cause_t cause, bool synced, uint32_t now_s)
{
    if (!synced) {
        return; // never-synced board: symmetric no-op, no storage I/O -- not even a lazy cache load
    }

    bb_system_reboot_budget_state_t *st = cache_get(cause);
    if (!st) return; // out-of-range cause -> nothing to record

    bb_system_reboot_budget_state_record(st, now_s);
    reboot_budget_save(cause, st);
}
