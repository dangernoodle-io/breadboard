// bb_pub core — transport-agnostic telemetry publisher.
// Compiled on both host (tests) and ESP-IDF (linked alongside bb_pub_espidf.c).
//
// Timestamp note: bb_pub_tick_once injects an "uptime_ms" field (uint64_t)
// using bb_clock_now_ms64(), which returns monotonic ms on all platforms
// (host: CLOCK_MONOTONIC, ESP-IDF: esp_timer_get_time()/1000). On devices
// with NTP synchronised wall-clock time, callers wanting epoch-ms should
// include the wall-clock timestamp themselves inside their sample_fn.
#include "bb_pub.h"
#include "bb_pub_defaults.h"
#include "bb_arena.h"
#include "bb_cache.h"
#include "bb_claim.h"
#include "bb_clock.h"
#include "bb_json.h"
#include "bb_log.h"
#include "bb_ntp.h"
#include "bb_nv.h"
#include "bb_nv_keys.h"
#include "bb_pool.h"

#include <inttypes.h>
#include <pthread.h>
#include <stdalign.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

// CONFIG_BB_PUB_MAX_SOURCES, CONFIG_BB_PUB_MAX_SINKS,
// CONFIG_BB_PUB_TOPIC_PREFIX, and CONFIG_BB_PUB_INTERVAL_MS are provided by
// Kconfig on ESP-IDF; supply defaults for host builds.
#ifndef CONFIG_BB_PUB_MAX_SOURCES
#define CONFIG_BB_PUB_MAX_SOURCES 16
#endif
#ifndef CONFIG_BB_PUB_MAX_SINKS
#define CONFIG_BB_PUB_MAX_SINKS 4
#endif
#ifndef CONFIG_BB_PUB_MAX_PAYLOAD_EXTENDERS
#define CONFIG_BB_PUB_MAX_PAYLOAD_EXTENDERS 4
#endif
#ifndef CONFIG_BB_PUB_SUBTOPIC_MAX
#define CONFIG_BB_PUB_SUBTOPIC_MAX 64
#endif
#ifndef CONFIG_BB_PUB_TOPIC_PREFIX
#define CONFIG_BB_PUB_TOPIC_PREFIX "metrics"
#endif
#ifndef CONFIG_BB_PUB_INTERVAL_MS
#define CONFIG_BB_PUB_INTERVAL_MS 10000
#endif

#ifndef CONFIG_BB_METRICS_PREFIX
#define CONFIG_BB_METRICS_PREFIX "bb"
#endif

// Telemetry-source scratch buffer size (bytes). A single static buffer is
// safe because only the bb_pub worker ever calls gather() concurrently (the
// gather loop runs under s_tick_lock, single-threaded per worker).
// CONFIG_BB_PUB_TELEM_SNAP_MAX host fallback lives in bb_pub_defaults.h.
// Worker-stack buffer for copying memoized serialized JSON out of bb_cache in
// Phase 2b (UAF-safe copy-out — no caller holds the cache-owned pointer).
#ifndef CONFIG_BB_PUB_TELEM_SERIALIZE_MAX
#define CONFIG_BB_PUB_TELEM_SERIALIZE_MAX 512
#endif

// Compile-time coupling invariant: every pub source registers a bb_cache topic,
// so the cache must be able to hold at least as many topics as there are sources.
_Static_assert(BB_CACHE_MAX_TOPICS >= CONFIG_BB_PUB_MAX_SOURCES,
    "BB_CACHE_MAX_TOPICS must be >= BB_PUB_MAX_SOURCES "
    "(each telemetry source registers a cache topic at init; raise "
    "CONFIG_BB_CACHE_MAX_TOPICS or lower CONFIG_BB_PUB_MAX_SOURCES)");

// Interval bounds (must match Kconfig range).
#define BB_PUB_INTERVAL_MS_MIN   1000UL
#define BB_PUB_INTERVAL_MS_MAX   3600000UL

#define BB_METRICS_PREFIX_MAX 64

// NVS namespace (bb_nv_namespaces.h) and keys (bb_nv_keys.h) used by bb_pub
// for its own persistent config are the SSOT shared constants included above.

static const char *TAG = "bb_pub";

// ---------------------------------------------------------------------------
// Internal state
// ---------------------------------------------------------------------------

typedef struct {
    char            subtopic[CONFIG_BB_PUB_SUBTOPIC_MAX];
    bb_pub_sample_fn fn;
    void            *ctx;
    uint32_t        last_sample_ms;
    bool            sampled_ever;
    char            tags[BB_PUB_MAX_TAGS_PER_SOURCE][BB_PUB_TAG_MAX + 1];
    const char     *tag_ptrs[BB_PUB_MAX_TAGS_PER_SOURCE]; /* pointers into tags[][] */
    int             ntags;
    // When true: source is managed by the telem gather pipeline.  Skip it
    // in the normal per-source tick loop; delivery is handled by the telem
    // Phase 1 / Phase 2 loops.  The adapter sample_fn is still enumerable
    // via bb_pub_source_info() and /api/telemetry/metrics.
    bool            telem_managed;
} bb_pub_source_t;

static bb_pub_source_t s_sources[CONFIG_BB_PUB_MAX_SOURCES];
static int             s_source_count   = 0;
static bool            s_hwm_warned     = false;

// ---------------------------------------------------------------------------
// Telemetry source registry (gather-into-cache pipeline)
// ---------------------------------------------------------------------------

typedef struct {
    char                     topic[CONFIG_BB_PUB_SUBTOPIC_MAX];
    bb_pub_gather_fn         gather;
    size_t                   snap_size;
    bb_pub_telemetry_flags_t flags;
    void                    *ctx;
    bool                     retain;
    bb_pub_cadence_t         cadence;
    // Cadence state — read/written only in Phase 2b of bb_pub_tick_once, which
    // runs on the single bb_pub worker task outside s_tick_lock.  No concurrent
    // writer exists; no new lock is needed.
    uint64_t                 last_pub_hash;
    bool                     published_once;
} bb_pub_telem_entry_t;

static bb_pub_telem_entry_t s_telem_sources[CONFIG_BB_PUB_MAX_SOURCES];
static int                  s_telem_count = 0;

// Single static scratch buffer for gather().  Safe because only one bb_pub
// worker thread ever calls gather() and the gather loop runs under s_tick_lock.
static uint8_t s_snap_scratch[CONFIG_BB_PUB_TELEM_SNAP_MAX];

static bb_pub_sink_t   s_sinks[CONFIG_BB_PUB_MAX_SINKS];
static int             s_sink_count     = 0;

// ---------------------------------------------------------------------------
// Payload-extender registry
// ---------------------------------------------------------------------------

typedef struct {
    bb_pub_payload_fn fn;
    void             *ctx;
} bb_pub_payload_entry_t;

static bb_pub_payload_entry_t s_payload_extenders[CONFIG_BB_PUB_MAX_PAYLOAD_EXTENDERS];
static int                    s_payload_extender_count = 0;
static bool                   s_payload_hwm_warned     = false;

// ---------------------------------------------------------------------------
// Runtime config (NVS-persisted)
// ---------------------------------------------------------------------------

// Effective interval in ms; loaded from NVS at first get, default = compile-time.
static uint32_t s_interval_ms  = 0;   /* 0 = not yet loaded */
// Persistent enable toggle; 1 = enabled (default).
static uint8_t  s_enabled      = 1;
// Whether s_interval_ms has been loaded from NVS.
static bool     s_config_loaded = false;

// Optional hook for live timer re-arm; set by the ESP-IDF worker.
static void (*s_interval_apply_hook)(uint32_t ms) = NULL;

// Load interval + enabled from NVS into the in-RAM cache (idempotent).
static void ensure_config_loaded(void)
{
    if (s_config_loaded) return;
    bb_nv_get_u32(BB_PUB_NVS_NS, BB_PUB_NVS_KEY_INTERVAL, &s_interval_ms,
                  (uint32_t)CONFIG_BB_PUB_INTERVAL_MS);
    bb_nv_get_u8(BB_PUB_NVS_NS, BB_PUB_NVS_KEY_ENABLED, &s_enabled, 1);
    s_config_loaded = true;
}

// ---------------------------------------------------------------------------
// Pause state
// ---------------------------------------------------------------------------

// s_paused is read/written under s_tick_lock.
// s_in_publish counts ongoing sink fan-out passes (0 or 1 in practice).
// bb_pub_pause() sets s_paused (stops new ticks) under s_tick_lock, then
// waits on s_in_publish_cv for s_in_publish to reach zero — bounded by
// CONFIG_BB_MQTT_NETWORK_TIMEOUT_MS (or BB_PUB_PAUSE_TIMEOUT_MS_DEFAULT).
// This allows the tick lock to be released before the blocking sink fan-out,
// so bb_pub_pause() no longer stalls for the full TLS-publish duration.
//
// Lock ordering (must be obeyed to avoid deadlock):
//   s_tick_lock is never acquired while s_in_publish_mu is held.
//   s_in_publish_mu may be acquired while s_tick_lock is held (for the brief
//   s_in_publish increment/decrement), but s_tick_lock is released before the
//   bounded-wait on s_in_publish_cv.
static bool s_paused = false;

// Tick lock — held only for the sample → serialize phase of tick_once (NOT
// across the blocking sk->publish / buffer_replay calls).  bb_pub_pause() sets
// s_paused under this lock then waits on s_in_publish_cv instead of
// barriering on this lock for the full publish duration.
static pthread_mutex_t s_tick_lock = PTHREAD_MUTEX_INITIALIZER;

// In-publish flag: incremented just before the blocking sink fan-out begins,
// decremented (and cv broadcast) just after it completes.
// Protected by s_in_publish_mu.
static pthread_mutex_t s_in_publish_mu  = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  s_in_publish_cv  = PTHREAD_COND_INITIALIZER;
static int             s_in_publish     = 0;

// Fallback pause-wait timeout for host builds where
// CONFIG_BB_MQTT_NETWORK_TIMEOUT_MS may not be defined.
// Matches the Kconfig default (30 000 ms).
#ifndef CONFIG_BB_MQTT_NETWORK_TIMEOUT_MS
#define BB_PUB_PAUSE_TIMEOUT_MS_DEFAULT 30000
#else
#define BB_PUB_PAUSE_TIMEOUT_MS_DEFAULT CONFIG_BB_MQTT_NETWORK_TIMEOUT_MS
#endif

// ---------------------------------------------------------------------------
// Status state
// ---------------------------------------------------------------------------

static bool     s_last_publish_ok  = false;
static uint32_t s_last_publish_ms  = 0;
static bool     s_published_ever   = false;
static bool     s_ring_size_warned = false;
// True once bb_pub_mark_started() is called (set by the ESP-IDF platform after
// bb_pub_start() succeeds). Remains false on host builds and AUTOREGISTER=n
// builds — correctly signals that the periodic worker is not running.
static bool     s_started          = false;

// ---------------------------------------------------------------------------
// Store-and-forward buffer (CONFIG_BB_PUB_BUFFER_ENABLE)
// ---------------------------------------------------------------------------
//
// Entry wire format (packed into a single bb_pool FIFO push):
//   [topic bytes (NUL-terminated)] [NUL separator] [payload bytes]
//
// The ring max_slot_bytes =
//   BB_PUB_BUFFER_TOPIC_MAX (192) + 1 (NUL sep) + CONFIG_BB_PUB_BUFFER_MAX_PAYLOAD_BYTES
//
// ring entry `ts`  = capture epoch-ms when NTP-synced, else 0 (uptime-ms
//                    not used here because bb_pool ts is int64 and uptime-ms
//                    would be ambiguous at replay; 0 signals "unknown epoch").
// ring entry `id`  = 0 (unused; reserved for future sequencing).
//
// Replay: on a tick where sink[0] delivers successfully and the ring is
// non-empty, drain oldest-first via peek → inject captured_ms if ts>0 →
// publish to sink[0] → pop on success, stop on failure.
//
// Only sink[0] is used for capture and replay to avoid double-buffering
// the legacy fan-out set.

#if CONFIG_BB_PUB_BUFFER_ENABLE

#ifndef CONFIG_BB_PUB_BUFFER_MAX_ENTRIES
#define CONFIG_BB_PUB_BUFFER_MAX_ENTRIES 16
#endif
#ifndef CONFIG_BB_PUB_BUFFER_MAX_PAYLOAD_BYTES
#define CONFIG_BB_PUB_BUFFER_MAX_PAYLOAD_BYTES 256
#endif
// Idle-free threshold: C default matches Kconfig default (30 ticks).
#ifndef CONFIG_BB_PUB_BUFFER_IDLE_FREE_TICKS
#define CONFIG_BB_PUB_BUFFER_IDLE_FREE_TICKS 30
#endif

// Topic budget: tunable via CONFIG_BB_PUB_BUFFER_TOPIC_MAX (Kconfig).
// Must accommodate CONFIG_BB_PUB_TOPIC_PREFIX + hostname + CONFIG_BB_PUB_SUBTOPIC_MAX + 2 seps + NUL.
#ifndef CONFIG_BB_PUB_BUFFER_TOPIC_MAX
#define CONFIG_BB_PUB_BUFFER_TOPIC_MAX 192
#endif
#define BB_PUB_BUFFER_TOPIC_MAX CONFIG_BB_PUB_BUFFER_TOPIC_MAX
// Total per-entry capacity.
#define BB_PUB_BUFFER_ENTRY_MAX \
    (BB_PUB_BUFFER_TOPIC_MAX + 1 + CONFIG_BB_PUB_BUFFER_MAX_PAYLOAD_BYTES)

// Ring storage (B1-489): two backing modes selected by
// CONFIG_BB_PUB_BUFFER_STATIC.
//
//   STATIC=n (default) — LAZY HEAP-BACKED. The ring is a bb_pool FIFO that
//     owns a right-sized heap arena (bb_pool_create_owned, BB_POOL_BACKING_
//     HEAP), created on first use and destroyed (bb_pool_destroy, freeing the
//     owned arena) after BB_PUB_BUFFER_IDLE_FREE_TICKS idle ticks post-drain
//     — ~0 standing RAM cost when the sink is healthy. A failed allocation
//     (fragmented heap) fails soft: the ring is unavailable for that cycle
//     and retried on the next outage.
//
//   STATIC=y — permanent static-BSS bb_arena (the B1-478 PR D shape),
//     carved once and never freed; deterministic standing cost, no heap
//     allocation ever, no idle-free reclaim (nothing to reclaim).
//
// Arena sizing (STATIC path only): pool-struct + FIFO-ring overhead is
// conservatively bounded (512 B pool/arena struct headroom + 64 B per-entry
// header/alignment overhead), then capacity * entry payload.  bb_pool_create()
// validates the arena has enough free space at creation time and logs + fails
// soft (ring_pool_get() returns NULL) if this bound is ever wrong for a given
// platform's struct layout. The lazy-heap path does not need this macro —
// bb_pool_create_owned() sizes its own arena via bb_pool_arena_size_needed().
#define BB_PUB_RING_POOL_ARENA_BYTES \
    (512u + \
     (size_t)(CONFIG_BB_PUB_BUFFER_MAX_ENTRIES) * 64u + \
     (size_t)(CONFIG_BB_PUB_BUFFER_MAX_ENTRIES) * (size_t)(BB_PUB_BUFFER_ENTRY_MAX))

// The static-BSS arena is compiled in whenever it might be exercised: always
// under CONFIG_BB_PUB_BUFFER_STATIC=y, and also under BB_PUB_TESTING so host
// tests can flip between both backing modes in the same binary via
// bb_pub_test_set_buffer_static().
#if CONFIG_BB_PUB_BUFFER_STATIC || defined(BB_PUB_TESTING)
static uint8_t    s_ring_arena_buf[BB_PUB_RING_POOL_ARENA_BYTES]
    __attribute__((aligned(_Alignof(max_align_t))));
static bb_arena_t s_ring_arena = NULL;
#endif
static bb_pool_t  s_ring_pool  = NULL;

// Tracks the ACTUAL backing of the current s_ring_pool object (true = carved
// from s_ring_arena via bb_pool_create; false = owns a heap arena via
// bb_pool_create_owned). This is deliberately separate from
// buffer_static_mode()'s runtime-overridable answer: under BB_PUB_TESTING,
// bb_pub_test_reset() is called both by Unity's global setUp() (before every
// test) and again by each test's own buf_reset() helper, and the test-seam
// override is reset to "-1 = compile default" partway through that sequence.
// Deciding *cleanup* behaviour by re-evaluating buffer_static_mode() at each
// reset point can therefore disagree with which backing the CURRENT pool
// object actually has (e.g. destroying a shared-arena pool via
// bb_pool_destroy(), or skipping the arena rewind a shared-arena pool needs
// before its single-use arena can be reused). Always consult this flag —
// set alongside pool creation — instead of re-deriving the mode when
// deciding how to tear the current pool down.
static bool       s_ring_pool_is_static = false;

// Runtime toggle for the static-BSS vs lazy-heap ring backing (mirrors
// CONFIG_BB_PUB_BUFFER_STATIC). Under BB_PUB_TESTING a test seam overrides
// the compile-time default so both modes can be exercised in the same binary.
#ifdef BB_PUB_TESTING
static int s_buffer_static_override = -1;   /* -1 = use compile-time default */

void bb_pub_test_set_buffer_static(bool is_static)
{
    s_buffer_static_override = is_static ? 1 : 0;
}

static bool buffer_static_mode(void)
{
    if (s_buffer_static_override >= 0) return s_buffer_static_override != 0;
#if CONFIG_BB_PUB_BUFFER_STATIC
    return true;
#else
    return false;
#endif
}
#else
static bool buffer_static_mode(void)
{
#if CONFIG_BB_PUB_BUFFER_STATIC
    return true;
#else
    return false;
#endif
}
#endif /* BB_PUB_TESTING */

// Set once when a lazy-heap allocation attempt fails, so a sustained outage
// logs the warning once instead of every tick; cleared on the next
// successful create. Only meaningful in lazy-heap mode.
static bool s_ring_alloc_fail_logged = false;

// Consecutive idle-tick counter (on-failure mode only). Counts publish ticks
// where the ring pool is non-NULL and empty. Reset to 0 whenever the ring
// gains entries. Protected by s_tick_lock (Phase 3). In lazy-heap mode,
// reaching the threshold destroys the pool (bb_pool_destroy) and frees its
// owned heap arena. In static mode there is nothing to reclaim, so reaching
// the threshold is a no-op. bb_pub_buffer_stats() is unaffected either way
// (count/dropped are sourced straight from the pool).
static uint32_t  s_buffer_idle_ticks = 0;

// Runtime toggle for always-on mode (mirrors CONFIG_BB_PUB_BUFFER_ALWAYS).
// Under BB_PUB_TESTING a test seam overrides the compile-time default so both
// modes can be exercised in the same binary.
#ifdef BB_PUB_TESTING
static int s_buffer_always = -1;   /* -1 = use compile-time default */

void bb_pub_test_set_buffer_always(bool always_on)
{
    s_buffer_always = always_on ? 1 : 0;
}

static bool buffer_always_on(void)
{
    if (s_buffer_always >= 0) return s_buffer_always != 0;
#if CONFIG_BB_PUB_BUFFER_ALWAYS
    return true;
#else
    return false;
#endif
}
#else
static bool buffer_always_on(void)
{
#if CONFIG_BB_PUB_BUFFER_ALWAYS
    return true;
#else
    return false;
#endif
}
#endif /* BB_PUB_TESTING */

// Idle-free threshold accessor (ticks; 0 = disabled).
// Under BB_PUB_TESTING a test seam can lower the threshold for fast tests.
#ifdef BB_PUB_TESTING
static int s_idle_free_ticks_override = -1;   /* -1 = use compile-time default */

void bb_pub_test_set_idle_free_ticks(int n)
{
    s_idle_free_ticks_override = n;
}

static uint32_t buffer_idle_free_ticks(void)
{
    if (s_idle_free_ticks_override >= 0) return (uint32_t)s_idle_free_ticks_override;
    return (uint32_t)CONFIG_BB_PUB_BUFFER_IDLE_FREE_TICKS;
}
#else
static uint32_t buffer_idle_free_ticks(void)
{
    return (uint32_t)CONFIG_BB_PUB_BUFFER_IDLE_FREE_TICKS;
}
#endif /* BB_PUB_TESTING */

// Lazily create the ring pool on first enqueue.
//
// STATIC mode: arena struct lives in s_ring_arena_buf, not heap; after
// bb_pub_test_reset() s_ring_pool is NULL but s_ring_arena may still be
// valid — in that case skip bb_arena_init and just re-create the pool from
// the already-reset arena.
//
// Lazy-heap mode (default): bb_pool_create_owned() allocates a right-sized
// heap arena and the pool struct in one shot; a failed allocation (e.g.
// fragmented no-PSRAM heap) fails soft — the ring is unavailable this cycle
// and ring_pool_get() is retried on the next outage.
static bb_pool_t ring_pool_get(void)
{
    if (s_ring_pool) return s_ring_pool;

    bb_pool_cfg_t cfg = {
        .mode           = BB_POOL_MODE_FIFO,
        .capacity       = (size_t)CONFIG_BB_PUB_BUFFER_MAX_ENTRIES,
        .max_slot_bytes = (size_t)BB_PUB_BUFFER_ENTRY_MAX,
        .full_policy    = BB_POOL_FULL_EVICT_OLDEST,
        .name           = "pub",
    };

    if (buffer_static_mode()) {
#if CONFIG_BB_PUB_BUFFER_STATIC || defined(BB_PUB_TESTING)
        if (!s_ring_arena) {
            bb_err_t ae = bb_arena_init(&s_ring_arena, s_ring_arena_buf,
                                         sizeof(s_ring_arena_buf));
            if (ae != BB_OK) {
                bb_log_w(TAG, "store-and-forward: arena init failed (%d)", ae);
                return NULL;
            }
        }
        bb_err_t e = bb_pool_create(&cfg, s_ring_arena, &s_ring_pool);
        if (e != BB_OK) {
            bb_log_w(TAG, "store-and-forward: pool create failed (%d)", e);
            s_ring_pool = NULL;
        } else {
            s_ring_pool_is_static = true;
        }
#endif
        return s_ring_pool;
    }

    bb_err_t e = bb_pool_create_owned(&cfg, BB_POOL_BACKING_HEAP, &s_ring_pool);
    if (e != BB_OK) {
        s_ring_pool = NULL;
        if (!s_ring_alloc_fail_logged) {
            bb_log_w(TAG, "store-and-forward: heap alloc failed (%d); "
                          "ring unavailable this cycle, retrying next outage", e);
            s_ring_alloc_fail_logged = true;
        }
    } else {
        s_ring_alloc_fail_logged = false;
        s_ring_pool_is_static    = false;
    }
    return s_ring_pool;
}

// Eagerly allocate the ring pool for always-on mode.
// Called once at init (espidf start) when CONFIG_BB_PUB_BUFFER_ALWAYS=y so
// the pool exists from boot.  Idempotent (ring_pool_get() is safe to call
// multiple times). In lazy-heap mode the pool stays allocated for the life
// of the process once always-on is enqueuing every tick (no idle window to
// reclaim it); this trades ~0-BSS for one standing heap allocation instead
// of a permanent static-BSS carve.
void bb_pub_buffer_init_eager(void)
{
    if (buffer_always_on()) {
        ring_pool_get();   /* allocate now; logs warning on failure */
    }
}

// Capture epoch accessor.
// Returns the current wall-clock epoch in milliseconds when NTP is synced,
// otherwise 0. A ts==0 in the ring means "epoch unknown at capture time";
// the receiver uses arrival time instead of a backdated point.
#ifdef BB_PUB_TESTING
// Test hook: override synced epoch for deterministic tests.
static int64_t s_test_epoch_ms = -1;   /* -1 = not overridden */

void bb_pub_test_set_synced_epoch_ms(int64_t epoch_ms)
{
    s_test_epoch_ms = epoch_ms;
}

static int64_t capture_epoch_ms(void)
{
    if (s_test_epoch_ms >= 0) {
        return s_test_epoch_ms;
    }
    if (bb_ntp_is_synced()) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    }
    return 0;
}
#else
static int64_t capture_epoch_ms(void)
{
    if (!bb_ntp_is_synced()) return 0;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
#endif /* BB_PUB_TESTING */

// Push topic+payload into the ring.
// Lock invariant: relies on the single-worker guarantee (bb_pool is not
// internally locked).  In always-on mode this is called from Phase 1, under
// s_tick_lock.  In on-failure mode this is called from Phase 2, with NO lock
// held.  Either way only one worker thread ever calls it concurrently.
// Returns true if the entry was successfully enqueued, false if it was rejected
// (topic too long or entry too large for a ring slot).  Callers in always-on
// mode use the return value to fall back to a direct publish on rejection so
// oversized entries are not silently dropped.
static bool buffer_capture(const char *topic, const char *payload, int payload_len)
{
    bb_pool_t pool = ring_pool_get();
    if (!pool) return false;

    size_t topic_len = strlen(topic);
    if (topic_len >= BB_PUB_BUFFER_TOPIC_MAX) {
        bb_log_d(TAG, "buffer: topic too long (%zu), not buffered", topic_len);
        return false;
    }

    // Pack: topic + NUL + payload
    size_t entry_len = topic_len + 1 + (size_t)payload_len;
    if (entry_len > BB_PUB_BUFFER_ENTRY_MAX) {
        bb_log_d(TAG, "buffer: entry too large (%zu > %zu), not buffered",
                 entry_len, (size_t)BB_PUB_BUFFER_ENTRY_MAX);
        return false;
    }

    // Static scratch (NOT stack): BB_PUB_BUFFER_ENTRY_MAX = TOPIC_MAX + 1 +
    // MAX_PAYLOAD_BYTES, which consumers raise well past the old 449 (e.g. 705
    // at MAX_PAYLOAD_BYTES=512). A 705-byte frame overflows the bb_pub worker's
    // CONFIG_BB_PUB_WORKER_STACK (4096 on heap-tight no-PSRAM boards),
    // corrupting adjacent heap and crashing later in bb_pool_push. Safe as
    // static: only the single bb_pub worker thread ever calls buffer_capture
    // (see the lock invariant above).
    static char entry[BB_PUB_BUFFER_ENTRY_MAX];
    memcpy(entry, topic, topic_len);
    entry[topic_len] = '\0';
    if (payload_len > 0) {
        memcpy(entry + topic_len + 1, payload, (size_t)payload_len);
    }

    int64_t ts = capture_epoch_ms();
    bb_err_t err = bb_pool_push(pool, entry, entry_len, ts, 0);
    if (err != BB_OK) {
        bb_log_w(TAG, "buffer: ring push failed (%d)", err);
    } else {
        bb_log_d(TAG, "buffer: captured '%s' (epoch_ms=%lld, ring=%zu)",
                 topic, (long long)ts, bb_pool_count(pool));
    }
    return true;
}

// Splice captured_ms into a JSON payload when ts > 0.
// Inserts `"captured_ms":<ts>,` right after the opening '{'.
// Returns a heap-allocated string that the caller must free with free(), or
// NULL if no injection needed (ts == 0) — caller uses original payload then.
// Also returns NULL on alloc failure — caller falls back to original.
static char *inject_captured_ms(const char *payload, int payload_len, int64_t ts)
{
    if (ts <= 0) return NULL;
    if (payload_len < 2 || payload[0] != '{') return NULL;

    // Build: {"captured_ms":<ts>,<rest after '{'>
    // field = "captured_ms":NNNNNNNNNNNNNN,  (max 14 digits + overhead = ~22 chars)
    char field[40];
    int field_len = snprintf(field, sizeof(field), "\"captured_ms\":%lld,", (long long)ts);
    if (field_len < 0 || (size_t)field_len >= sizeof(field)) return NULL;

    int out_len = 1 + field_len + payload_len - 1;  /* '{' + field + rest */
    char *out = malloc((size_t)out_len + 1);
    if (!out) return NULL;

    out[0] = '{';
    memcpy(out + 1, field, (size_t)field_len);
    // rest = payload[1..payload_len-1]
    memcpy(out + 1 + field_len, payload + 1, (size_t)(payload_len - 1));
    out[out_len] = '\0';
    return out;
}

// Replay buffered entries to sink[0].
// Lock invariant: always called from Phase 2, with NO lock held (s_tick_lock
// was released before Phase 2 begins; s_in_publish_mu is NOT held during the
// body of Phase 2).  Relies on the single-worker guarantee (bb_pool is not
// internally locked) — only one worker thread ever runs this concurrently.
// In on-failure mode (always_on=false) this is called only after a successful
// live-publish tick.  In always-on mode it is called unconditionally every tick.
//
// now_epoch_ms     — current wall-clock epoch in ms (0 if NTP not synced).
// delay_threshold_ms — when > 0, captured_ms is injected ONLY if the entry's
//                    age (now - entry_ts) exceeds this threshold. When 0,
//                    captured_ms is injected for any entry with entry_ts > 0
//                    (on-failure mode: every buffered entry is by definition
//                    delayed, so no age gate is needed).
//
// In always-on mode the caller passes 1.5 × interval_ms so that fresh entries
// drained the same tick they were enqueued carry no captured_ms (age ≈ 0),
// while entries that survived a real outage across ticks do carry it.
// In on-failure mode the caller passes 0 to preserve the prior behavior.
static void buffer_replay(const bb_pub_sink_t *sink,
                          int64_t now_epoch_ms, int64_t delay_threshold_ms)
{
    bb_pool_t pool = s_ring_pool;
    if (!pool || bb_pool_count(pool) == 0) return;

    // Replay buffer: peek content size, deliver, pop on success.
    // Stack-allocated scratch (max 192+1+256+1 = 450 bytes; safe on the
    // worker stack which is >= 4096 bytes even in plaintext-only builds).
    // Previously declared static, but buffer_replay is now called from Phase 2
    // with no lock held — the static would be safe only under the
    // single-worker guarantee, but stack allocation is clearer about intent
    // and avoids BSS waste.
    char replay_buf[BB_PUB_BUFFER_ENTRY_MAX + 1];

    for (;;) {
        size_t   entry_len = 0;
        int64_t  entry_ts  = 0;
        uint32_t entry_id  = 0;

        bb_err_t err = bb_pool_peek_oldest(pool,
                                           replay_buf, sizeof(replay_buf) - 1,
                                           &entry_len, &entry_ts, &entry_id);
        if (err == BB_ERR_NOT_FOUND) break;  /* ring empty */
        if (err != BB_OK) {
            bb_log_w(TAG, "buffer: peek failed (%d), stopping replay", err);
            break;
        }

        // Parse topic (NUL-terminated at replay_buf[topic_len]).
        replay_buf[entry_len] = '\0';
        const char *topic   = replay_buf;
        size_t topic_len    = strlen(topic);
        const char *payload = (topic_len + 1 < entry_len)
                              ? replay_buf + topic_len + 1
                              : "";
        int payload_len     = (int)(entry_len - topic_len - 1);
        if (payload_len < 0) payload_len = 0;

        // Determine whether to inject captured_ms.
        //   - delay_threshold_ms == 0 (on-failure mode): inject whenever
        //     entry_ts > 0, regardless of age (every buffered entry is
        //     delayed by definition — it failed at least one prior tick).
        //   - delay_threshold_ms > 0 (always-on mode): inject only when the
        //     entry is genuinely old; same-tick fresh entries are not stamped.
        int64_t inject_ts = 0;
        if (entry_ts > 0 && now_epoch_ms > 0) {
            if (delay_threshold_ms == 0) {
                inject_ts = entry_ts;   /* on-failure: always stamp */
            } else {
                int64_t age_ms = now_epoch_ms - entry_ts;
                if (age_ms > delay_threshold_ms) {
                    inject_ts = entry_ts;
                }
            }
        }

        char *patched = inject_captured_ms(payload, payload_len, inject_ts);
        const char *pub_payload = patched ? patched : payload;
        int         pub_len     = patched ? (int)strlen(patched) : payload_len;

        bb_err_t deliver = sink->publish(sink->ctx, topic, pub_payload, pub_len, false);
        free(patched);   /* NULL-safe */

        if (deliver != BB_OK) {
            bb_log_w(TAG, "buffer: replay delivery failed for '%s': %d", topic, deliver);
            break;   /* sink still down; leave rest in ring */
        }

        bb_log_d(TAG, "buffer: replayed '%s' (epoch_ms=%lld, inject_ts=%lld)",
                 topic, (long long)entry_ts, (long long)inject_ts);
        bb_pool_pop_oldest(pool);
    }
}

#else  /* !CONFIG_BB_PUB_BUFFER_ENABLE */

// Stubs so callers always link cleanly when the buffer feature is compiled out.
#ifdef BB_PUB_TESTING
void bb_pub_test_set_synced_epoch_ms(int64_t epoch_ms) { (void)epoch_ms; }
void bb_pub_test_set_buffer_always(bool always_on)     { (void)always_on; }
void bb_pub_test_set_idle_free_ticks(int n)            { (void)n; }
#endif
void bb_pub_buffer_init_eager(void) {}

#endif /* CONFIG_BB_PUB_BUFFER_ENABLE */

// ---------------------------------------------------------------------------
// Public API — buffer stats
// ---------------------------------------------------------------------------

void bb_pub_buffer_stats(bb_pub_buffer_stats_t *out)
{
    if (!out) return;
#if CONFIG_BB_PUB_BUFFER_ENABLE
    bb_pool_t pool = s_ring_pool;
    if (pool) {
        out->count     = bb_pool_count(pool);
        out->dropped   = bb_pool_dropped(pool);
        out->truncated = 0;  /* bb_pool has no truncation counter; oversized
                                 entries are already rejected by
                                 buffer_capture's length pre-check, so this
                                 was always 0 even pre-migration */
    } else {
        out->count     = 0;
        out->dropped   = 0;
        out->truncated = 0;
    }
#else
    out->count     = 0;
    out->dropped   = 0;
    out->truncated = 0;
#endif
}

// ---------------------------------------------------------------------------
// Public API — sinks
// ---------------------------------------------------------------------------

bb_err_t bb_pub_add_sink(const bb_pub_sink_t *sink)
{
    if (!sink || !sink->publish) return BB_ERR_INVALID_ARG;

    if (s_sink_count >= CONFIG_BB_PUB_MAX_SINKS) {
        return BB_ERR_NO_SPACE;
    }
    s_sinks[s_sink_count++] = *sink;
    return BB_OK;
}

void bb_pub_clear_sinks(void)
{
    s_sink_count = 0;
}

bb_err_t bb_pub_set_sink(const bb_pub_sink_t *sink)
{
    bb_pub_clear_sinks();
    if (!sink || !sink->publish) {
        return BB_OK;
    }
    return bb_pub_add_sink(sink);
}

// ---------------------------------------------------------------------------
// Public API — status
// ---------------------------------------------------------------------------

bb_err_t bb_pub_get_status(bb_pub_status_t *out)
{
    if (!out) return BB_ERR_INVALID_ARG;
    // s_source_count and s_sink_count are only mutated at registration time
    // (before the worker starts); no lock needed for them.
    out->source_count = s_source_count;
    out->sink_count   = s_sink_count;
    // s_last_publish_ok, s_last_publish_ms, and s_published_ever are written
    // by Phase 3 of bb_pub_tick_once under s_tick_lock.  Acquire the lock for
    // the read so callers get a coherent snapshot.
    pthread_mutex_lock(&s_tick_lock);
    out->last_publish_ok = s_last_publish_ok;
    out->last_publish_ms = s_last_publish_ms;
    out->published_ever  = s_published_ever;
    pthread_mutex_unlock(&s_tick_lock);
    // s_started is written once at startup (before any tick); no lock needed.
    out->available = s_started;
    return BB_OK;
}

// ---------------------------------------------------------------------------
// Public API — source enumeration (used by bb_telemetry for /api/telemetry/metrics)
// ---------------------------------------------------------------------------

int bb_pub_source_count(void)
{
    return s_source_count;
}

bb_err_t bb_pub_source_info(int i, const char **subtopic, bb_pub_sample_fn *fn,
                             void **ctx, uint32_t *last_sample_ms, bool *sampled_ever)
{
    if (i < 0 || i >= s_source_count) return BB_ERR_INVALID_ARG;
    bb_pub_source_t *src = &s_sources[i];
    if (subtopic)      *subtopic      = src->subtopic;
    if (fn)            *fn            = src->fn;
    if (ctx)           *ctx           = src->ctx;
    if (last_sample_ms) *last_sample_ms = src->last_sample_ms;
    if (sampled_ever)  *sampled_ever  = src->sampled_ever;
    return BB_OK;
}

bb_err_t bb_pub_source_info_ex(int i, const char **subtopic, bb_pub_sample_fn *fn,
                                void **ctx, uint32_t *last_sample_ms, bool *sampled_ever,
                                const char *const **tags, int *ntags)
{
    bb_err_t err = bb_pub_source_info(i, subtopic, fn, ctx, last_sample_ms, sampled_ever);
    if (err != BB_OK) return err;
    if (tags)  *tags  = s_sources[i].tag_ptrs;
    if (ntags) *ntags = s_sources[i].ntags;
    return BB_OK;
}

bool bb_pub_ring_undersized(void)
{
#if CONFIG_BB_PUB_BUFFER_ENABLE
    return buffer_always_on() &&
           CONFIG_BB_PUB_BUFFER_MAX_ENTRIES < s_source_count;
#else
    return false;
#endif
}

bb_err_t bb_pub_sink_info(int i, const char **out_transport, bool *out_tls)
{
    pthread_mutex_lock(&s_tick_lock);
    if (i < 0 || i >= s_sink_count) {
        pthread_mutex_unlock(&s_tick_lock);
        return BB_ERR_INVALID_ARG;
    }
    if (out_transport) *out_transport = s_sinks[i].transport;
    if (out_tls)       *out_tls       = s_sinks[i].tls;
    pthread_mutex_unlock(&s_tick_lock);
    return BB_OK;
}

// ---------------------------------------------------------------------------
// Lock-free internal helpers — MUST be called with s_tick_lock held.
// Used by meta_gather (telem Phase 1, under s_tick_lock) to avoid re-
// acquiring the non-recursive mutex and self-deadlocking.
// External callers must use bb_pub_get_status / bb_pub_sink_info instead.
// ---------------------------------------------------------------------------

bb_err_t bb_pub_get_status_nolock(bb_pub_status_t *out)
{
    if (!out) return BB_ERR_INVALID_ARG;
    // s_source_count and s_sink_count: registration-time only, safe without lock.
    out->source_count    = s_source_count;
    out->sink_count      = s_sink_count;
    // s_last_publish_ok / _ms / _published_ever: caller holds s_tick_lock.
    out->last_publish_ok = s_last_publish_ok;
    out->last_publish_ms = s_last_publish_ms;
    out->published_ever  = s_published_ever;
    // s_started: written once at startup before any tick; safe without lock.
    out->available       = s_started;
    return BB_OK;
}

bb_err_t bb_pub_sink_info_nolock(int i, const char **out_transport, bool *out_tls)
{
    // s_sinks[] / s_sink_count: registration-time only; caller holds s_tick_lock.
    if (i < 0 || i >= s_sink_count) return BB_ERR_INVALID_ARG;
    if (out_transport) *out_transport = s_sinks[i].transport;
    if (out_tls)       *out_tls       = s_sinks[i].tls;
    return BB_OK;
}

// ---------------------------------------------------------------------------
// Prometheus metric-name prefix
// ---------------------------------------------------------------------------

static char s_metrics_prefix[BB_METRICS_PREFIX_MAX] = CONFIG_BB_METRICS_PREFIX;

void bb_pub_set_metrics_prefix(const char *prefix)
{
    if (!prefix) return;
    strncpy(s_metrics_prefix, prefix, BB_METRICS_PREFIX_MAX - 1);
    s_metrics_prefix[BB_METRICS_PREFIX_MAX - 1] = '\0';
}

const char *bb_pub_metrics_prefix(void)
{
    return s_metrics_prefix;
}

// ---------------------------------------------------------------------------
// Public API — sources
// ---------------------------------------------------------------------------

bb_err_t bb_pub_register_source(const char *subtopic, bb_pub_sample_fn fn, void *ctx)
{
    if (!subtopic || !fn) return BB_ERR_INVALID_ARG;

    if (s_source_count >= CONFIG_BB_PUB_MAX_SOURCES) {
        bb_log_w(TAG, "source registry full (%d/%d): '%s' dropped",
                 s_source_count, CONFIG_BB_PUB_MAX_SOURCES, subtopic);
        return BB_ERR_NO_SPACE;
    }

    // High-watermark warning at cap-1.
    if (!s_hwm_warned && s_source_count == CONFIG_BB_PUB_MAX_SOURCES - 1) {
        bb_log_w(TAG, "source registry at high-watermark (%d/%d)",
                 s_source_count, CONFIG_BB_PUB_MAX_SOURCES);
        s_hwm_warned = true;
    }

    bb_pub_source_t *src = &s_sources[s_source_count++];
    memset(src, 0, sizeof(*src));   /* zero ALL fields including telem_managed */
    strncpy(src->subtopic, subtopic, sizeof(src->subtopic) - 1);
    src->subtopic[sizeof(src->subtopic) - 1] = '\0';
    src->fn  = fn;
    src->ctx = ctx;
    return BB_OK;
}

bb_err_t bb_pub_register_source_ex(const char *subtopic, bb_pub_sample_fn fn, void *ctx,
                                    const char *const *tags, int ntags)
{
    bb_err_t err = bb_pub_register_source(subtopic, fn, ctx);
    if (err != BB_OK) return err;
    bb_pub_source_t *src = &s_sources[s_source_count - 1];
    int n = (ntags < BB_PUB_MAX_TAGS_PER_SOURCE) ? ntags : BB_PUB_MAX_TAGS_PER_SOURCE;
    for (int t = 0; t < n; t++) {
        strncpy(src->tags[t], tags[t], BB_PUB_TAG_MAX);
        src->tags[t][BB_PUB_TAG_MAX] = '\0';
        src->tag_ptrs[t] = src->tags[t];
    }
    src->ntags = n;
    return BB_OK;
}

// ---------------------------------------------------------------------------
// Telemetry adapter: reads the cached snapshot; used by /api/telemetry/metrics
// and any caller of bb_pub_sample_into().  ctx is the stable topic pointer
// stored in s_telem_sources[i].topic.
// ---------------------------------------------------------------------------

static bool _telem_adapter_sample(bb_json_t obj, void *ctx)
{
    return bb_cache_serialize_into((const char *)ctx, obj) == BB_OK;
}

// ---------------------------------------------------------------------------
// FNV-1a 64-bit hash — used by the ON_CHANGE cadence gate in Phase 2b.
// ---------------------------------------------------------------------------

static uint64_t fnv1a64(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint64_t h = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < len; i++) {
        h ^= (uint64_t)p[i];
        h *= UINT64_C(1099511628211);
    }
    return h;
}

// ---------------------------------------------------------------------------
// bb_pub_deliver_to_sinks — fan a pre-serialized payload to all sinks.
// Used by Phase 2b (telem path).  The caller has already serialized ONCE.
// Does NOT re-serialize.  Does NOT inject transport/tls (B1-388).
//
// Returns count of subscribed sinks that failed delivery (0 = all succeeded).
// Sinks skipped by their subscribe predicate are not counted as failures.
// ---------------------------------------------------------------------------

static int bb_pub_deliver_to_sinks(const char *full_topic,
                                    const char *subtopic,
                                    const char *json,
                                    int json_len,
                                    bool retain,
                                    const bb_pub_sink_t *sinks,
                                    int sink_count)
{
    int failed = 0;
    for (int si = 0; si < sink_count; si++) {
        const bb_pub_sink_t *sk = &sinks[si];
        if (sk->subscribe &&
            !sk->subscribe(subtopic, NULL, 0, sk->subscribe_ctx)) {
            continue;
        }
        bb_err_t err = sk->publish(sk->ctx, full_topic, json, json_len, retain);
        if (err != BB_OK) {
            bb_log_w(TAG, "deliver_to_sinks: sink[%d] failed for '%s': %d",
                     si, full_topic, err);
            failed++;
        }
    }
    return failed;
}

// ---------------------------------------------------------------------------
// bb_pub_register_telemetry
// ---------------------------------------------------------------------------

bb_err_t bb_pub_register_telemetry(const bb_pub_telemetry_cfg_t *cfg)
{
    if (!cfg || !cfg->topic || !cfg->gather || !cfg->serialize || cfg->snap_size == 0) {
        return BB_ERR_INVALID_ARG;
    }
    if (cfg->snap_size > CONFIG_BB_PUB_TELEM_SNAP_MAX) {
        bb_log_e(TAG, "register_telemetry: snap_size %zu > max %d for '%s'",
                 cfg->snap_size, CONFIG_BB_PUB_TELEM_SNAP_MAX, cfg->topic);
        return BB_ERR_NO_SPACE;
    }
    if (s_telem_count >= CONFIG_BB_PUB_MAX_SOURCES) {
        bb_log_w(TAG, "register_telemetry: telem table full for '%s'", cfg->topic);
        return BB_ERR_NO_SPACE;
    }

    // Determine bb_cache flags: SSE topic only when BB_PUB_TELEM_SSE is set.
    bb_cache_flags_t cache_flags =
        (cfg->flags & BB_PUB_TELEM_SSE) ? BB_CACHE_FLAG_SSE : BB_CACHE_FLAG_NONE;

    bb_cache_config_t cache_cfg = {
        .key       = cfg->topic,
        .snapshot  = NULL,
        .snap_size = cfg->snap_size,
        .serialize = cfg->serialize,
        .flags     = cache_flags,
    };
    bb_err_t err = bb_cache_register(&cache_cfg);
    if (err != BB_OK) {
        bb_log_w(TAG, "register_telemetry: bb_cache_register failed for '%s': %d",
                 cfg->topic, err);
        return err;
    }

    // Store in telem table before calling bb_pub_register_source so that if
    // register_source succeeds, the entry is already visible to the tick loop.
    bb_pub_telem_entry_t *te = &s_telem_sources[s_telem_count];
    strncpy(te->topic, cfg->topic, sizeof(te->topic) - 1);
    te->topic[sizeof(te->topic) - 1] = '\0';
    te->gather         = cfg->gather;
    te->snap_size      = cfg->snap_size;
    te->flags          = cfg->flags;
    te->ctx            = cfg->ctx;
    te->retain         = cfg->retain;
    te->cadence        = cfg->cadence;
    te->last_pub_hash  = 0;
    te->published_once = false;
    s_telem_count++;

    // Register the adapter as a legacy source so the topic appears in
    // bb_pub_source_count / source_info / bb_pub_sample_into.
    // ctx = pointer to the stable topic string in the telem table entry.
    err = bb_pub_register_source(cfg->topic, _telem_adapter_sample,
                                  (void *)te->topic);
    if (err != BB_OK) {
        // Undo the telem entry (registration order is: telem then source).
        s_telem_count--;
        bb_log_w(TAG, "register_telemetry: bb_pub_register_source failed for '%s': %d",
                 cfg->topic, err);
        return err;
    }

    // Mark the just-registered source as telem-managed so tick skips it in the
    // normal source loop (delivery is handled by telem Phase 1/2 instead).
    s_sources[s_source_count - 1].telem_managed = true;

    bb_log_i(TAG, "registered telem source '%s' (snap=%zu flags=0x%x)",
             cfg->topic, cfg->snap_size, (unsigned)cfg->flags);
    return BB_OK;
}

// ---------------------------------------------------------------------------
// Public API — payload extenders
// ---------------------------------------------------------------------------

bb_err_t bb_pub_register_payload_extender(bb_pub_payload_fn fn, void *ctx)
{
    if (!fn) return BB_ERR_INVALID_ARG;

    if (s_payload_extender_count >= CONFIG_BB_PUB_MAX_PAYLOAD_EXTENDERS) {
        bb_log_w(TAG, "payload extender registry full (%d/%d)",
                 s_payload_extender_count, CONFIG_BB_PUB_MAX_PAYLOAD_EXTENDERS);
        return BB_ERR_NO_SPACE;
    }

    // High-watermark warning at cap-1.
    if (!s_payload_hwm_warned &&
        s_payload_extender_count == CONFIG_BB_PUB_MAX_PAYLOAD_EXTENDERS - 1) {
        bb_log_w(TAG, "payload extender registry at high-watermark (%d/%d)",
                 s_payload_extender_count, CONFIG_BB_PUB_MAX_PAYLOAD_EXTENDERS);
        s_payload_hwm_warned = true;
    }

    s_payload_extenders[s_payload_extender_count].fn  = fn;
    s_payload_extenders[s_payload_extender_count].ctx = ctx;
    s_payload_extender_count++;
    return BB_OK;
}

// ---------------------------------------------------------------------------
// Public API — interval + enabled
// ---------------------------------------------------------------------------

bb_err_t bb_pub_set_interval_ms(uint32_t ms)
{
    if (ms < BB_PUB_INTERVAL_MS_MIN || ms > BB_PUB_INTERVAL_MS_MAX) {
        return BB_ERR_INVALID_ARG;
    }
    ensure_config_loaded();
    s_interval_ms = ms;
    bb_err_t err = bb_nv_set_u32(BB_PUB_NVS_NS, BB_PUB_NVS_KEY_INTERVAL, ms);
    if (err != BB_OK) {
        bb_log_w(TAG, "set_interval_ms: NVS write failed: %d", err);
    }
    if (s_interval_apply_hook) {
        s_interval_apply_hook(ms);
    }
    return BB_OK;
}

uint32_t bb_pub_get_interval_ms(void)
{
    ensure_config_loaded();
    return s_interval_ms;
}

bb_err_t bb_pub_set_enabled(bool en)
{
    ensure_config_loaded();
    s_enabled = en ? 1u : 0u;
    bb_err_t err = bb_nv_set_u8(BB_PUB_NVS_NS, BB_PUB_NVS_KEY_ENABLED, s_enabled);
    if (err != BB_OK) {
        bb_log_w(TAG, "set_enabled: NVS write failed: %d", err);
    }
    return BB_OK;
}

bool bb_pub_is_enabled(void)
{
    ensure_config_loaded();
    return s_enabled != 0;
}

void bb_pub_set_interval_apply_hook(void (*hook)(uint32_t ms))
{
    s_interval_apply_hook = hook;
}

void bb_pub_mark_started(void)
{
    s_started = true;
}

bb_err_t bb_pub_set_interval_volatile_ms(uint32_t ms)
{
    if (ms < BB_PUB_INTERVAL_MS_MIN || ms > BB_PUB_INTERVAL_MS_MAX) {
        return BB_ERR_INVALID_ARG;
    }
    // Re-arm the timer via the hook (if registered) WITHOUT touching NVS and
    // WITHOUT updating s_interval_ms so that bb_pub_get_interval_ms() continues
    // to return the persisted/configured value.
    if (s_interval_apply_hook) {
        s_interval_apply_hook(ms);
    }
    return BB_OK;
}

// ---------------------------------------------------------------------------
// Public API — pause / resume
// ---------------------------------------------------------------------------

void bb_pub_pause(void)
{
    // 1. Set s_paused under s_tick_lock so the next tick skips its body.
    pthread_mutex_lock(&s_tick_lock);
    s_paused = true;
    pthread_mutex_unlock(&s_tick_lock);

    // 2. Bounded-wait: if a publish is currently in flight (s_in_publish > 0),
    //    wait on the condvar until it completes OR the timeout elapses.
    //    This guarantees no concurrent TLS write when pause() returns in the
    //    common case, without holding s_tick_lock across the blocking publish.
    //
    //    Timeout = CONFIG_BB_MQTT_NETWORK_TIMEOUT_MS (default 30 000 ms).
    //    On timeout we log once and proceed — the OTA TLS will not race an
    //    already-completed publish; it may race a very slow one, but the
    //    no-concurrent-TLS guarantee degrades gracefully rather than deadlocking.
    pthread_mutex_lock(&s_in_publish_mu);
    if (s_in_publish > 0) {
        struct timespec abs_ts;
        clock_gettime(CLOCK_REALTIME, &abs_ts);
        long timeout_ms = BB_PUB_PAUSE_TIMEOUT_MS_DEFAULT;
        abs_ts.tv_sec  += timeout_ms / 1000;
        abs_ts.tv_nsec += (timeout_ms % 1000) * 1000000L;
        if (abs_ts.tv_nsec >= 1000000000L) {
            abs_ts.tv_sec++;
            abs_ts.tv_nsec -= 1000000000L;
        }
        while (s_in_publish > 0) {
            int rc = pthread_cond_timedwait(&s_in_publish_cv, &s_in_publish_mu,
                                            &abs_ts);
            if (rc != 0) {
                // ETIMEDOUT or spurious error — log once and break.
                bb_log_w(TAG,
                         "pause: timed out waiting for in-flight publish "
                         "(>%ld ms); proceeding (no-concurrent-tls guarantee "
                         "degraded for this pause)", timeout_ms);
                break;
            }
        }
    }
    pthread_mutex_unlock(&s_in_publish_mu);
}

void bb_pub_resume(void)
{
    // s_paused is only written under s_tick_lock for set (in pause()) and in
    // bb_pub_test_reset().  Clearing it here without the lock is safe: the
    // tick body checks it under the lock at entry, so the worst case is a
    // one-tick delay before the resume takes effect.
    s_paused = false;
}

bool bb_pub_is_paused(void)
{
    return s_paused;
}

// ---------------------------------------------------------------------------
// Subscription filter helpers
// ---------------------------------------------------------------------------

bool bb_pub_subscription_match(const bb_pub_subscription_t *sub,
                                const char *subtopic,
                                const char *const *tags, int ntags)
{
    if (!sub) return true;
    if (!sub->subtopics && !sub->tags) return true;

    if (sub->subtopics) {
        for (int i = 0; i < sub->nsubtopics; i++) {
            if (sub->subtopics[i] && strcmp(sub->subtopics[i], subtopic) == 0) return true;
        }
    }
    if (sub->tags) {
        for (int ti = 0; ti < ntags; ti++) {
            for (int si = 0; si < sub->ntags; si++) {
                if (sub->tags[si] && tags && tags[ti] &&
                    strcmp(sub->tags[si], tags[ti]) == 0) return true;
            }
        }
    }
    return false;
}

bool bb_pub_subscription_predicate(const char *subtopic,
                                   const char *const *tags, int ntags,
                                   void *ctx)
{
    return bb_pub_subscription_match((const bb_pub_subscription_t *)ctx,
                                     subtopic, tags, ntags);
}

// ---------------------------------------------------------------------------
// bb_pub_sample_into — call a single source's sample_fn by subtopic name
// ---------------------------------------------------------------------------

bool bb_pub_sample_into(const char *subtopic, bb_json_t obj)
{
    if (!subtopic || !obj) return false;
    for (int i = 0; i < s_source_count; i++) {
        if (strcmp(s_sources[i].subtopic, subtopic) == 0) {
            return s_sources[i].fn(obj, s_sources[i].ctx);
        }
    }
    bb_log_d(TAG, "sample_into: no source for '%s'", subtopic);
    return false;
}

// ---------------------------------------------------------------------------
// Public API — tick
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Per-sink payload snapshot (used by bb_pub_tick_once to build payloads
// under s_tick_lock before releasing it for the blocking publish phase).
// ---------------------------------------------------------------------------

typedef struct {
    char  topic[CONFIG_BB_PUB_SUBTOPIC_MAX + 128];
    char *json;                                    /* heap-allocated; freed after publish */
    int   json_len;
    int   sink_count;
    bool  subscribed[CONFIG_BB_PUB_MAX_SINKS];     /* which sinks pass the subscribe filter */
#if CONFIG_BB_PUB_BUFFER_ENABLE
    bool  buffer_enqueued;   /* true = captured into ring for sink[0] */
#endif
} tick_payload_t;

// Telem gather-result: which telem sources fired this tick.
typedef struct {
    int  telem_idx;   /* index into s_telem_sources[] */
    bool fired;       /* gather returned true */
} telem_result_t;

bb_err_t bb_pub_tick_once(void)
{
    ensure_config_loaded();
    if (!bb_pub_is_enabled()) return BB_OK;

    // Fast pre-checks before taking the lock.
    if (s_paused) return BB_OK;
    if (s_sink_count == 0) return BB_OK;

    // -----------------------------------------------------------------------
    // Phase 1 — under s_tick_lock: check paused, run sources, serialize JSON.
    // The lock is held only for CPU-bound work (no blocking I/O).
    // -----------------------------------------------------------------------
    pthread_mutex_lock(&s_tick_lock);

    // Re-check under the lock: bb_pub_pause() may have set s_paused between
    // the fast pre-check above and our lock acquisition.
    if (s_paused) {
        pthread_mutex_unlock(&s_tick_lock);
        return BB_OK;
    }

#if CONFIG_BB_PUB_BUFFER_ENABLE
    // One-shot guard: warn when always-on ring is smaller than the per-tick
    // source count.  In that case the ring evicts the first-enqueued sources
    // before the drain, causing silent live-data loss every tick.
    // Emitted once at the first tick (after registration is complete).
    if (!s_ring_size_warned && buffer_always_on() &&
        CONFIG_BB_PUB_BUFFER_MAX_ENTRIES < s_source_count) {
        s_ring_size_warned = true;
        bb_log_w(TAG,
                 "always-on ring (%d entries) < source count (%d): "
                 "first-enqueued sources are evicted before drain every tick "
                 "(silent live-data loss) — raise CONFIG_BB_PUB_BUFFER_MAX_ENTRIES",
                 CONFIG_BB_PUB_BUFFER_MAX_ENTRIES, s_source_count);
    }
#endif

    // Take ONE timestamp for the entire cycle (u64 — no 49.7-day wrap).
    uint64_t ts_ms = bb_clock_now_ms64();
#if CONFIG_BB_PUB_BUFFER_ENABLE
    // Capture current wall-clock epoch once (0 if NTP not synced).
    int64_t tick_epoch_ms = capture_epoch_ms();
    // Snapshot interval_ms here under s_tick_lock so Phase 2 (outside the
    // lock) doesn't race with bb_pub_set_interval_ms() from an HTTP handler.
    uint32_t snap_interval_ms = bb_pub_get_interval_ms();
#endif

    const char *hostname = bb_nv_config_hostname();
    if (!hostname || hostname[0] == '\0') {
        hostname = "device";
    }

    // Build per-source payload snapshots under the lock.
    // stack-allocate the array; source count is bounded by CONFIG_BB_PUB_MAX_SOURCES.
    tick_payload_t payloads[CONFIG_BB_PUB_MAX_SOURCES];
    int payload_count = 0;

    // Telem gather results (one per registered telem source).
    telem_result_t telem_results[CONFIG_BB_PUB_MAX_SOURCES];
    int telem_fired_count = 0;

    // Snapshot current sink count and sinks (shallow copy; publish fn pointers
    // are stable across the tick since registration only appends).
    int            snap_sink_count = s_sink_count;
    bb_pub_sink_t  snap_sinks[CONFIG_BB_PUB_MAX_SINKS];
    for (int si = 0; si < snap_sink_count; si++) {
        snap_sinks[si] = s_sinks[si];
    }

    // -----------------------------------------------------------------------
    // Telem Phase 1 (under s_tick_lock): gather each telem source into the
    // static scratch buffer, store it in bb_cache, mark it for Phase 2 fan-out.
    // Uses a single static scratch buffer — safe because the bb_pub worker is
    // the only caller (single-threaded gather → store sequence).
    // -----------------------------------------------------------------------
    for (int ti = 0; ti < s_telem_count; ti++) {
        bb_pub_telem_entry_t *te = &s_telem_sources[ti];
        memset(s_snap_scratch, 0, te->snap_size);
        bool gathered = te->gather(s_snap_scratch, te->ctx);
        telem_results[ti].telem_idx = ti;
        telem_results[ti].fired     = gathered;
        if (gathered) {
            bb_err_t err = bb_cache_update(&(bb_cache_update_t){ .key = te->topic, .snap = s_snap_scratch });
            if (err != BB_OK) {
                bb_log_w(TAG, "tick: bb_cache_update failed for '%s': %d",
                         te->topic, err);
                telem_results[ti].fired = false;
            } else {
                telem_fired_count++;
            }
        }
    }

    for (int i = 0; i < s_source_count; i++) {
        bb_pub_source_t *src = &s_sources[i];

        // Skip telem-managed sources — delivered via telem Phase 2 instead.
        if (src->telem_managed) continue;

        bb_json_t obj = bb_json_obj_new();
        if (!obj) {
            bb_log_w(TAG, "tick: failed to allocate JSON obj for '%s'", src->subtopic);
            continue;
        }

        bool do_publish = src->fn(obj, src->ctx);
        if (!do_publish) {
            bb_json_free(obj);
            continue;
        }

        // Record per-source liveness on the publish==true branch.
        src->last_sample_ms = ts_ms;
        src->sampled_ever   = true;

        // Inject shared timestamp field (uptime-ms u64; see file-level note above).
        bb_json_obj_set_number(obj, "uptime_ms", (double)ts_ms);

        // Invoke payload extenders in registration order before serialize.
        for (int pi = 0; pi < s_payload_extender_count; pi++) {
            s_payload_extenders[pi].fn(obj, src->subtopic,
                                       s_payload_extenders[pi].ctx);
        }

        tick_payload_t *p = &payloads[payload_count];
        snprintf(p->topic, sizeof(p->topic), "%s/%s/%s",
                 CONFIG_BB_PUB_TOPIC_PREFIX, hostname, src->subtopic);
        p->sink_count = snap_sink_count;
        p->json       = NULL;
        p->json_len   = 0;
#if CONFIG_BB_PUB_BUFFER_ENABLE
        p->buffer_enqueued = false;
#endif

        // Compute subscription bitmap (all sinks, before serialize).
        bool any_subscribed = false;
        for (int si = 0; si < snap_sink_count; si++) {
            const bb_pub_sink_t *sk = &snap_sinks[si];
            bool sub = (!sk->subscribe ||
                        sk->subscribe(src->subtopic, src->tag_ptrs, src->ntags,
                                      sk->subscribe_ctx));
            p->subscribed[si] = sub;
            if (sub) any_subscribed = true;
        }

        if (!any_subscribed) {
            bb_json_free(obj);
            continue;
        }

#if CONFIG_BB_PUB_LEGACY_TRANSPORT_INJECT
        // Legacy B1-388 behaviour: serialize ONCE PER SINK to stamp transport/tls.
        // This path is intentionally kept identical to the pre-B1-388 code for
        // consumers that explicitly opt in.
        bool any_sink_ok = false;
        // Re-use the new single-json field as a per-iteration temp; we promote to
        // the old multi-json model by serializing inside the sink loop.
        for (int si = 0; si < snap_sink_count; si++) {
            if (!p->subscribed[si]) continue;
            const bb_pub_sink_t *sk = &snap_sinks[si];

            bb_json_obj_delete_key(obj, "transport");
            bb_json_obj_delete_key(obj, "tls");
            if (sk->transport) {
                bb_json_obj_set_string(obj, "transport", sk->transport);
                bb_json_obj_set_bool(obj, "tls", sk->tls);
            }

            // For the legacy path only, we allocate a local json string and publish
            // it directly here rather than deferring to Phase 2.  Phase 2 will see
            // p->json==NULL and p->subscribed all false (reset below) and skip this
            // payload.  This means the always-on buffer-capture path is DISABLED
            // under legacy mode; a deliberate trade-off.
            char *json_si = bb_json_serialize(obj);
            if (!json_si) {
                bb_log_w(TAG, "tick(legacy): failed to serialize '%s' sink[%d]",
                         src->subtopic, si);
                continue;
            }
            // NOTE: publishing is deferred to outside the lock (Phase 2), so we
            // can't publish inline here.  Store the per-sink strings in a local
            // array and handle them in Phase 2 via a separate legacy path.
            //
            // For now, fall through to the non-legacy path and let the single json
            // cover all sinks (the transport/tls field will reflect the LAST sink
            // that has a non-NULL transport).  Consumers wanting true per-sink
            // transport injection should set BB_PUB_LEGACY_TRANSPORT_INJECT=n and
            // omit transport/tls from payloads, which is the recommended default.
            bb_json_free_str(json_si);
            any_sink_ok = true;
        }
        (void)any_sink_ok;  /* fall through to serialize-once below */
        // Reset transport/tls so serialize-once produces clean output.
        bb_json_obj_delete_key(obj, "transport");
        bb_json_obj_delete_key(obj, "tls");
#endif /* CONFIG_BB_PUB_LEGACY_TRANSPORT_INJECT */

        // B1-388: serialize ONCE for all sinks.
        //
        // Envelope (B1-570 PR-3): this legacy bb_pub_register_source path
        // delivers to sinks directly, bypassing bb_cache — so unlike the
        // telem/cache path (which gets its envelope from
        // bb_cache_get_serialized/bb_cache_post), the wire contract has to be
        // built here explicitly to keep every sink-delivered topic uniform:
        // {"ts_ms":<sample-time>,"data":{...}}. ts_ms reuses the single
        // per-cycle timestamp captured above (the same value already injected
        // as the "uptime_ms" data field) — this source's sample time.
        bb_json_t envelope = bb_json_obj_new();
        if (!envelope) {
            bb_log_w(TAG, "tick: failed to allocate envelope for '%s'", src->subtopic);
            bb_json_free(obj);
            continue;
        }
        bb_json_obj_set_int(envelope, "ts_ms", (int64_t)ts_ms);
        bb_json_obj_set_obj(envelope, "data", obj);  /* ownership of obj transfers */

        char *json = bb_json_serialize(envelope);
        bb_json_free(envelope);
        if (!json) {
            bb_log_w(TAG, "tick: failed to serialize '%s'", src->subtopic);
            continue;
        }
        int json_len = (int)strlen(json);

#if CONFIG_BB_PUB_BUFFER_ENABLE
        // Always-on mode: capture into ring for sink[0].  The ring makes its own
        // copy so json stays valid for direct delivery to sinks[1..n].
        if (p->subscribed[0] && buffer_always_on()) {
            if (buffer_capture(p->topic, json, json_len)) {
                p->buffer_enqueued = true;   /* sink[0] served by drain */
            } else {
                bb_log_d(TAG, "buffer: oversized entry will be published directly");
                /* p->buffer_enqueued stays false → sink[0] gets direct publish */
            }
        }
#endif

        p->json     = json;   /* ownership transferred; freed after publish */
        p->json_len = json_len;
        payload_count++;
    }

    // Release the tick lock before the blocking publish phase.
    pthread_mutex_unlock(&s_tick_lock);

    if (payload_count == 0 && telem_fired_count == 0) {
        return BB_OK;
    }

    // -----------------------------------------------------------------------
    // Phase 2 — outside s_tick_lock: blocking sink fan-out + buffer_replay.
    // Signal s_in_publish so bb_pub_pause() can bounded-wait.
    // -----------------------------------------------------------------------
    pthread_mutex_lock(&s_in_publish_mu);
    s_in_publish++;
    pthread_mutex_unlock(&s_in_publish_mu);

    bool tick_published = false;
    bool tick_all_ok    = true;

    // Phase 2a: normal source fan-out (serialize-once path).
    for (int i = 0; i < payload_count; i++) {
        tick_payload_t *p = &payloads[i];

        if (!p->json) {
#if CONFIG_BB_PUB_BUFFER_ENABLE
            if (p->buffer_enqueued) tick_published = true;
#endif
            continue;   /* serialization failed earlier (buffer_enqueued also handled) */
        }

        for (int si = 0; si < p->sink_count; si++) {
            if (!p->subscribed[si]) continue;
#if CONFIG_BB_PUB_BUFFER_ENABLE
            // sink[0] served by ring drain below; skip direct publish for it.
            if (si == 0 && p->buffer_enqueued) {
                tick_published = true;
                continue;
            }
#endif

            bb_err_t err = snap_sinks[si].publish(snap_sinks[si].ctx,
                                                   p->topic,
                                                   p->json,
                                                   p->json_len,
                                                   false);
            if (err != BB_OK) {
                bb_log_w(TAG, "sink[%d] publish failed for '%s': %d",
                         si, p->topic, err);
                tick_all_ok = false;
#if CONFIG_BB_PUB_BUFFER_ENABLE
                // On-failure mode: capture on sink[0] failure only.
                if (si == 0 && !buffer_always_on()) {
                    (void)buffer_capture(p->topic, p->json, p->json_len);
                }
#endif
            }
            tick_published = true;
        }

        bb_json_free_str(p->json);
        p->json = NULL;
    }

    // Phase 2b: telem source fan-out — SSE and sink delivery.
    // SSOT guarantee: serialize ONCE per telem topic (memoized in bb_cache),
    // COPY it out once into a scratch buffer, then share that single copy
    // with both SSE (bb_cache_post_serialized) and sinks (bb_pub_deliver_to_sinks).
    // Static is safe under the single-worker guarantee: bb_pub_tick_once is
    // never called concurrently (see buffer_capture comment at ~L353).  Moving
    // off the stack removes CONFIG_BB_PUB_TELEM_SERIALIZE_MAX (1024 B) from the
    // bb_pub task stack, preventing overflow on C3/S3 tight-stack builds.
    static char telem_buf[CONFIG_BB_PUB_TELEM_SERIALIZE_MAX];
    for (int ti = 0; ti < s_telem_count; ti++) {
        if (!telem_results[ti].fired) continue;
        bb_pub_telem_entry_t *te = &s_telem_sources[telem_results[ti].telem_idx];

        // Copy-out under the cache entry lock: the serializer ran (at most) once
        // via the cache's dirty flag (set by bb_cache_update in Phase 1).  SSE,
        // every sink, and any REST poll all receive these exact same enveloped
        // bytes ({"ts_ms":N,"data":{...}}, B1-570 PR-3) — bb_cache freezes
        // ts_ms + the memoized "data" bytes between updates, so REST == SSE ==
        // sink byte-for-byte.  Holding only our own copy makes the fan-out
        // UAF-safe against a concurrent re-serialize.
        size_t   S_len = 0;
        bb_err_t ge = bb_cache_get_serialized(te->topic, telem_buf,
                                              sizeof(telem_buf), &S_len);
        if (ge != BB_OK) {
            bb_log_d(TAG, "telem Phase2b: get_serialized failed for '%s': %d",
                     te->topic, ge);
            continue;
        }
        const char *S = telem_buf;

        // SSE: post the copied string (no re-serialization).
        if (te->flags & BB_PUB_TELEM_SSE) {
            bb_err_t e = bb_cache_post_serialized(te->topic, S, S_len);
            if (e != BB_OK) {
                bb_log_w(TAG, "telem Phase2b: SSE post failed for '%s': %d",
                         te->topic, e);
            }
        }

        // SINKS: fan the same copied string to all subscribing sinks.
        // Cadence gate: EVERY_TICK always delivers; ON_CHANGE only when content
        // changed; ONCE only on first publish.  SSE (above) is unconditional.
        //
        // Cadence state commits only on successful delivery to all subscribed
        // sinks; a partial/total failure re-attempts next tick (retained/once
        // values are idempotent).  A persistently-failing subscribed sink
        // causes re-attempts each tick for ON_CHANGE/ONCE — bounded by the
        // tick interval and correct for retain semantics.
        if ((te->flags & BB_PUB_TELEM_SINKS) && snap_sink_count > 0) {
            char full_topic[CONFIG_BB_PUB_SUBTOPIC_MAX + 128];
            snprintf(full_topic, sizeof(full_topic), "%s/%s/%s",
                     CONFIG_BB_PUB_TOPIC_PREFIX, hostname, te->topic);
            switch (te->cadence) {
            case BB_PUB_CADENCE_EVERY_TICK:
                bb_pub_deliver_to_sinks(full_topic, te->topic, S, (int)S_len,
                                        te->retain, snap_sinks, snap_sink_count);
                break;
            case BB_PUB_CADENCE_ON_CHANGE: {
                uint64_t h = fnv1a64(S, S_len);
                if (h != te->last_pub_hash) {
                    int failed = bb_pub_deliver_to_sinks(full_topic, te->topic,
                                                         S, (int)S_len,
                                                         te->retain, snap_sinks,
                                                         snap_sink_count);
                    if (failed == 0) {
                        te->last_pub_hash = h;
                    }
                }
                break;
            }
            case BB_PUB_CADENCE_ONCE:
                if (!te->published_once) {
                    int failed = bb_pub_deliver_to_sinks(full_topic, te->topic,
                                                         S, (int)S_len,
                                                         te->retain, snap_sinks,
                                                         snap_sink_count);
                    if (failed == 0) {
                        te->published_once = true;
                    }
                }
                break;
            }
        }

        tick_published = true;
    }

#if CONFIG_BB_PUB_BUFFER_ENABLE
    if (buffer_always_on()) {
        if (snap_sink_count > 0) {
            // snap_interval_ms was captured under s_tick_lock in Phase 1.
            int64_t threshold_ms = (int64_t)snap_interval_ms + (int64_t)snap_interval_ms / 2;
            buffer_replay(&snap_sinks[0], tick_epoch_ms, threshold_ms);
        }
    } else {
        if (tick_all_ok && tick_published && snap_sink_count > 0) {
            buffer_replay(&snap_sinks[0], tick_epoch_ms, 0);
        }
    }
#endif

    // Free any payloads that didn't get consumed (e.g. serialization error path).
    for (int i = 0; i < payload_count; i++) {
        if (payloads[i].json) {
            bb_json_free_str(payloads[i].json);
            payloads[i].json = NULL;
        }
    }

    // Signal that publish phase is complete.
    pthread_mutex_lock(&s_in_publish_mu);
    s_in_publish--;
    pthread_cond_broadcast(&s_in_publish_cv);
    pthread_mutex_unlock(&s_in_publish_mu);

    // -----------------------------------------------------------------------
    // Phase 3 — re-acquire s_tick_lock to write status fields, then release.
    // -----------------------------------------------------------------------
    pthread_mutex_lock(&s_tick_lock);

#if CONFIG_BB_PUB_BUFFER_ENABLE
    if (buffer_always_on()) {
        if (tick_published) {
            s_last_publish_ms = ts_ms;
            s_published_ever  = true;
            bool drained = (s_ring_pool == NULL || bb_pool_count(s_ring_pool) == 0);
            s_last_publish_ok = drained;
        }
    } else {
        if (tick_published) {
            s_last_publish_ms  = ts_ms;
            s_published_ever   = true;
            s_last_publish_ok  = tick_all_ok;
        }
        // Idle-tick bookkeeping (B1-419 / B1-489): after BB_PUB_BUFFER_
        // IDLE_FREE_TICKS consecutive empty publish ticks post-drain, reclaim
        // the ring. In the default lazy-heap mode this actually destroys the
        // pool (bb_pool_destroy frees its owned heap arena), restoring ~0
        // standing RAM cost until the next outage recreates it via
        // ring_pool_get(). In static mode there is no heap allocation to
        // reclaim, so this is a no-op — the counter is retained purely so the
        // bb_pub_test_set_idle_free_ticks() test hook and its API contract
        // stay intact; bb_pub_buffer_stats() is unaffected either way
        // (count/dropped are sourced straight from the pool).
        uint32_t idle_thresh = buffer_idle_free_ticks();
        if (idle_thresh > 0 && s_ring_pool != NULL) {
            if (bb_pool_count(s_ring_pool) == 0) {
                s_buffer_idle_ticks++;
                if (s_buffer_idle_ticks >= idle_thresh) {
                    s_buffer_idle_ticks = 0;
                    if (!s_ring_pool_is_static) {
                        bb_pool_destroy(s_ring_pool);
                        s_ring_pool = NULL;
                    }
                }
            } else {
                s_buffer_idle_ticks = 0;
            }
        }
    }
#else
    if (tick_published) {
        s_last_publish_ms  = ts_ms;
        s_published_ever   = true;
        s_last_publish_ok  = tick_all_ok;
    }
#endif

    pthread_mutex_unlock(&s_tick_lock);
    return BB_OK;
}

// ---------------------------------------------------------------------------
// Exclusive-sink arbiter (backed by bb_claim)
// ---------------------------------------------------------------------------

static bb_claim_t s_sink_claim = BB_CLAIM_INIT;

bb_err_t bb_pub_exclusive_acquire(const char *sink_id)
{
    return bb_claim_acquire(&s_sink_claim, sink_id);
}

void bb_pub_exclusive_release(const char *sink_id)
{
    bb_claim_release(&s_sink_claim, sink_id);
}

// ---------------------------------------------------------------------------
// Testing hooks
// ---------------------------------------------------------------------------

#ifdef BB_PUB_TESTING
void bb_pub_exclusive_reset(void)
{
    bb_claim_reset(&s_sink_claim);
}

void bb_pub_test_reset(void)
{
    memset(s_sources, 0, sizeof(s_sources));   /* zero ALL fields including telem_managed */
    s_source_count             = 0;
    s_telem_count              = 0;
    memset(s_telem_sources, 0, sizeof(s_telem_sources));
    s_hwm_warned               = false;
#if CONFIG_BB_PUB_BUFFER_ENABLE
    s_ring_size_warned         = false;
#endif
    s_sink_count               = 0;
    s_last_publish_ok          = false;
    s_last_publish_ms          = 0;
    s_published_ever           = false;
    s_started                  = false;
    s_paused                   = false;
    s_payload_extender_count   = 0;
    s_payload_hwm_warned       = false;
    // Reset runtime config to defaults.
    s_interval_ms              = (uint32_t)CONFIG_BB_PUB_INTERVAL_MS;
    s_enabled                  = 1;
    s_config_loaded            = true;   /* bypass NVS for tests */
    s_interval_apply_hook      = NULL;
    bb_claim_reset(&s_sink_claim);
    strncpy(s_metrics_prefix, CONFIG_BB_METRICS_PREFIX, BB_METRICS_PREFIX_MAX - 1);
    s_metrics_prefix[BB_METRICS_PREFIX_MAX - 1] = '\0';
#if CONFIG_BB_PUB_BUFFER_ENABLE
    // Decide cleanup by the ACTUAL backing of the current s_ring_pool
    // (s_ring_pool_is_static), NOT by re-evaluating buffer_static_mode() here.
    // bb_pub_test_reset() is called twice per test under BB_PUB_TESTING —
    // once by Unity's global setUp() and again by each test's own buf_reset()
    // helper — and the test-seam override is reset to "-1 = compile default"
    // by the first call. A mode re-derived at the second call can therefore
    // disagree with which backing the live pool object actually has, causing
    // e.g. bb_pool_destroy() on a shared-arena pool (wrong — arena pools are
    // never owned by bb_pool) or skipping the arena rewind a shared-arena
    // pool needs before its single-use arena can back a second pool.
    //
    // The pool is intentionally NOT eagerly recreated here — it is left NULL
    // (with the static arena's bump offset rewound, if applicable) and
    // recreated lazily on next use via ring_pool_get(), which re-derives the
    // mode fresh from whatever the upcoming test explicitly sets. This also
    // resets cumulative diagnostic counters (dropped) to zero between tests
    // without touching the global heap in static mode.
    if (s_ring_pool_is_static) {
        if (s_ring_arena) {
            bb_arena_reset(s_ring_arena);
        }
        s_ring_pool = NULL;
    } else if (s_ring_pool) {
        bb_pool_destroy(s_ring_pool);
        s_ring_pool = NULL;
    }
    s_test_epoch_ms            = -1;
    s_buffer_always            = -1;   /* revert to compile-time default */
    s_buffer_idle_ticks        = 0;
    s_idle_free_ticks_override = -1;   /* revert to compile-time default */
    s_buffer_static_override   = -1;   /* revert to compile-time default */
    s_ring_alloc_fail_logged   = false;
#endif
    // Re-initialise the tick lock and in-publish primitives so any lock state
    // from a prior test is cleared.
    pthread_mutex_destroy(&s_tick_lock);
    pthread_mutex_init(&s_tick_lock, NULL);
    pthread_mutex_destroy(&s_in_publish_mu);
    pthread_mutex_init(&s_in_publish_mu, NULL);
    pthread_cond_destroy(&s_in_publish_cv);
    pthread_cond_init(&s_in_publish_cv, NULL);
    s_in_publish = 0;
}
#endif
