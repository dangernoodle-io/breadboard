#pragma once

// bb_cache — canonical state-key cache + shared serializer.
//
// ONE registered serializer per key guarantees that SSE event payloads and
// REST handler payloads are identical by construction: both call the same
// bb_cache_serialize_fn against the same canonical struct.
//
// Ownership model (tri-state, keyed off snapshot/snap_size):
//   snapshot == NULL, snap_size >  0  — OWNED. bb_cache owns the struct;
//                       caller copies in via bb_cache_update().
//   snapshot != NULL, snap_size == 0  — GETTER. key owns the struct; bb_cache
//                       reads through the getter on every read; snap_size is
//                       ignored; bb_cache_update is a no-op.
//   snapshot != NULL, snap_size >  0  — OWNED+FALLBACK (cold-start seed,
//                       PR-4a-0). Behaves exactly like OWNED (bb_cache_update
//                       writes it, memoized/dirty-gated reads) EXCEPT: while
//                       the entry is unpopulated (no bb_cache_update() call
//                       has landed yet), a read/serialize invokes snapshot()
//                       ONCE to seed the owned buffer so the endpoint is never
//                       empty at cold start. Strict boot-race bridge: no
//                       expiry, and it never re-runs once a real write lands
//                       (bb_cache_update()'s has_value/changed semantics are
//                       driven only by real writes -- the seed does not set
//                       has_value, so the first real write still reports
//                       changed=true unconditionally, exactly like plain
//                       OWNED mode). Only meaningful for keys that have a
//                       writer -- never register a getter-only (writerless)
//                       key with a non-zero snap_size expecting this
//                       behavior; that is simply GETTER mode with a wasted
//                       allocation.
//
// Envelope contract (B1-570 PR-3, BREAKING wire change).
// bb_cache_get_serialized() and bb_cache_post() wrap the serializer's output
// as {"ts_ms": <sample-time-ms>, "data": {...}} -- "data" is exactly what
// cfg->serialize wrote (producers do NOT emit their own ts_ms field anymore).
// bb_cache owns the timestamp: owned-mode keys are stamped at
// bb_cache_update() (right after the copy-in, or overridden via
// bb_cache_update_t.ts_ms); getter-mode keys are stamped each time the
// snapshot() getter is invoked (the read IS the sample).
// bb_cache_serialize_into() does NOT apply the envelope -- it is the
// embed-a-key-as-a-section primitive (see its own doc comment) and emits raw
// "data" fields only.

#include "bb_core.h"
#include "bb_json.h"

// ---------------------------------------------------------------------------
// Capacity constants (Kconfig bridge — pattern from bb_clock.h)
// ---------------------------------------------------------------------------
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#ifdef CONFIG_BB_CACHE_MAX_TOPICS
#define BB_CACHE_MAX_TOPICS CONFIG_BB_CACHE_MAX_TOPICS
#endif
#ifdef CONFIG_BB_CACHE_KEY_MAX
#define BB_CACHE_KEY_MAX CONFIG_BB_CACHE_KEY_MAX
#endif
#endif
#ifndef BB_CACHE_MAX_TOPICS
#define BB_CACHE_MAX_TOPICS 32
#endif
#ifndef BB_CACHE_KEY_MAX
#define BB_CACHE_KEY_MAX 96
#endif

// ---------------------------------------------------------------------------
// Age-out eviction backstop sweep (Kconfig bridge — pattern from bb_clock.h)
// ---------------------------------------------------------------------------
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#ifdef CONFIG_BB_CACHE_SWEEP_ENABLE
#define BB_CACHE_SWEEP_ENABLE CONFIG_BB_CACHE_SWEEP_ENABLE
#endif
#ifdef CONFIG_BB_CACHE_SWEEP_PERIOD_MS
#define BB_CACHE_SWEEP_PERIOD_MS CONFIG_BB_CACHE_SWEEP_PERIOD_MS
#endif
#endif
#ifndef BB_CACHE_SWEEP_ENABLE
#define BB_CACHE_SWEEP_ENABLE 0
#endif
#ifndef BB_CACHE_SWEEP_PERIOD_MS
#define BB_CACHE_SWEEP_PERIOD_MS 30000
#endif

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Capability flags
// ---------------------------------------------------------------------------

/** Flags passed to bb_cache_config_t.flags to control per-key behaviour. */
typedef uint32_t bb_cache_flags_t;

/** No special flags — owned struct, no SSE event topic registered. */
#define BB_CACHE_FLAG_NONE  (0u)
/** Register an SSE event topic so bb_cache_post can fan out to SSE clients. */
#define BB_CACHE_FLAG_SSE   (1u << 0)

// ---------------------------------------------------------------------------
// Age-out eviction (B1-592 A3)
// ---------------------------------------------------------------------------

// Classification of an entry's age against its configured stale/evict
// windows. Pure -- no locks, no clock reads, no I/O -- so it is directly
// host-testable and shared verbatim between the LAZY (read-time) and SWEEP
// (periodic backstop) eviction paths.
typedef enum {
    BB_CACHE_ENTRY_FRESH,
    BB_CACHE_ENTRY_STALE,
    BB_CACHE_ENTRY_EVICT,
} bb_cache_entry_state_t;

// Classify age_ms against the entry's configured windows:
//   age_ms <  stale_age_ms                       -> FRESH
//   stale_age_ms <= age_ms < evict_age_ms         -> STALE
//   age_ms >= evict_age_ms                        -> EVICT
// stale_age_ms == 0 means "no stale window" -- the entry stays FRESH until
// it crosses evict_age_ms (never reports STALE).
bb_cache_entry_state_t bb_cache_evaluate_age(uint64_t age_ms, uint32_t stale_age_ms,
                                              uint32_t evict_age_ms);

// Per-key eviction policy. Default (zero-initialized, BB_CACHE_EVICT_PINNED)
// preserves today's behavior -- a registered key is never auto-freed.
typedef enum {
    BB_CACHE_EVICT_PINNED = 0,  // never auto-evicted (today's behavior)
    BB_CACHE_EVICT_AGE_OUT,     // evicted once unread/unwritten past evict_age_ms
} bb_cache_evict_policy_t;

// Eviction configuration, embedded in bb_cache_config_t.
//
//   policy        — BB_CACHE_EVICT_PINNED (default) or BB_CACHE_EVICT_AGE_OUT.
//   stale_age_ms  — AGE_OUT only. Age at which reads should be treated as
//                   stale-but-still-served by the caller's own staleness
//                   check (see bb_cache_is_stale()). 0 means no stale
//                   window. Ignored when policy is PINNED.
//   evict_age_ms  — AGE_OUT only. Age at which the entry is evicted (freed)
//                   on next read (LAZY) or the next sweep pass (SWEEP
//                   backstop -- enable via CONFIG_BB_CACHE_SWEEP_ENABLE).
//                   Must be > 0 and > stale_age_ms -- see
//                   bb_cache_register()'s AGE_OUT validation.
typedef struct {
    bb_cache_evict_policy_t policy;
    uint32_t                stale_age_ms;
    uint32_t                evict_age_ms;
} bb_cache_eviction_t;

// ---------------------------------------------------------------------------
// Serializer type
// ---------------------------------------------------------------------------

// Serializer fn: writes fields from *snap into obj.
// Called under the per-entry lock with a valid snap pointer.
typedef void (*bb_cache_serialize_fn)(bb_json_t obj, const void *snap);

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

// Configuration for a registered cache entry.
//
//   key        — registry identity string (e.g. "net.health"). COPIED into
//                the registry (up to BB_CACHE_KEY_MAX-1 chars, NUL-
//                terminated) — the caller does NOT need to keep it alive
//                past the bb_cache_register() call. Becomes the wire SSE
//                event topic only when flags has BB_CACHE_FLAG_SSE.
//   snapshot   — nullable getter fn: returns a pointer to the canonical
//                struct. Pass NULL for plain OWNED mode (bb_cache owns the
//                struct; copy in via bb_cache_update()). Pass non-NULL with
//                snap_size == 0 for plain GETTER mode. Pass non-NULL with
//                snap_size > 0 for OWNED+FALLBACK (cold-start seed) -- see
//                the tri-state ownership model above.
//   snap_size  — sizeof the owned struct. Required (> 0) whenever bb_cache
//                should own a buffer for this key -- i.e. snapshot == NULL
//                (plain OWNED, mandatory) or snapshot != NULL (OWNED+FALLBACK,
//                optional). Ignored (may be 0) for plain GETTER mode.
//   serialize  — serializer fn invoked by both post and serialize_into.
//   flags      — BB_CACHE_FLAG_* bitmask. Use BB_CACHE_FLAG_SSE to also
//                register an event topic for SSE fan-out via bb_cache_post.
//                Use BB_CACHE_FLAG_NONE for sink-only entries that do not
//                need SSE delivery. Zero-initializing flags is equivalent
//                to BB_CACHE_FLAG_NONE — callers that need SSE MUST set
//                this explicitly.
//   eviction   — bb_cache_eviction_t. Zero-initializing (policy ==
//                BB_CACHE_EVICT_PINNED) preserves today's never-auto-free
//                behavior. BB_CACHE_EVICT_AGE_OUT is only valid on OWNED
//                entries (snapshot == NULL) -- see bb_cache_register()'s
//                validation below.
typedef struct {
    const char             *key;
    const void            *(*snapshot)(void);
    size_t                  snap_size;
    bb_cache_serialize_fn   serialize;
    bb_cache_flags_t        flags;
    bb_cache_eviction_t     eviction;
} bb_cache_config_t;

// Register a cache entry.
//
// Returns BB_ERR_INVALID_ARG if cfg, cfg->key, or cfg->serialize is NULL, or
// if strlen(cfg->key) >= BB_CACHE_KEY_MAX (over-length keys are rejected
// loudly at register time, never silently truncated).
// Returns BB_ERR_NO_SPACE if the registry is full, or (owned mode) the
// snapshot buffer could not be allocated.
// Idempotent: registering an already-registered key returns BB_OK without
// creating a duplicate entry.
//
// AGE_OUT eviction validation (B1-592 A3, cfg->eviction.policy ==
// BB_CACHE_EVICT_AGE_OUT only -- BB_CACHE_EVICT_PINNED is unrestricted):
// Returns BB_ERR_INVALID_ARG if cfg->eviction.evict_age_ms == 0, if
// cfg->eviction.stale_age_ms >= cfg->eviction.evict_age_ms, or if
// cfg->snapshot != NULL (getter/refresh mode re-stamps ts_ms to now on every
// read, so age-out is meaningless there -- AGE_OUT is only valid on OWNED
// entries).
bb_err_t bb_cache_register(const bb_cache_config_t *cfg);

// Config-struct + explicit first-time-reporting variant of bb_cache_register().
//
// Performs the identical find-or-init atomically under a SINGLE lock
// acquisition and additionally reports, via out_first_time (nullable), whether
// THIS call performed the key's first-time registration (true) or merely
// observed an already-registered key and returned early (false). Exists so a
// caller that needs atomic first-time detection (e.g. bb_cache_reactive's
// on_register firing) never has to pair a separate bb_cache_exists() probe
// with bb_cache_register() -- two SEPARATE lock acquisitions leave a TOCTOU
// window where two racing first-time registers of the same key could both
// observe "not yet registered" and both fire on_register, violating the
// exactly-once contract.
// bb_cache_register(cfg) is a thin wrapper: bb_cache_register_ex(cfg, NULL).
// Same validation/error contract as bb_cache_register() (see above);
// out_first_time is left untouched on error other than being defaulted to
// false at entry.
bb_err_t bb_cache_register_ex(const bb_cache_config_t *cfg, bool *out_first_time);

// Configuration for a bb_cache_update() call.
//
//   key          — registered key (required).
//   snap         — pointer to the struct to copy in (required).
//   out_changed  — optional (NULL ok). Owned / owned+fallback mode (key has
//                  an owned buffer): set true when *snap differs (memcmp)
//                  from the previously stored bytes, OR this is the first
//                  REAL write since the key was registered (no false
//                  negative against a zero-initialized owned buffer). This
//                  holds even when a OWNED+FALLBACK cold-start seed already
//                  populated the buffer via a prior read -- the seed never
//                  sets the has_value flag this check relies on, so the
//                  first real write still reports changed=true
//                  unconditionally, regardless of whether *snap happens to
//                  match the seeded bytes. The comparison happens BEFORE the
//                  copy-in. Getter mode (no owned buffer): always set false
//                  (bb_cache has no owned bytes to diff against).
//   ts_ms        — 0 (default) stamps the envelope sample-time as now
//                  (bb_clock_now_ms64()); nonzero overrides the stamp with a
//                  caller-supplied sample time (e.g. an ingress source's own
//                  timestamp, or a self-emit source's capture time).
typedef struct {
    const char *key;
    const void *snap;
    bool       *out_changed;
    int64_t     ts_ms;
} bb_cache_update_t;

// Copy req->snap into the owned struct under the per-entry lock.
// No-op (returns BB_OK) when the key was registered as plain GETTER mode
// (snapshot != NULL, no owned buffer) -- out_changed is set false in that
// case. Works for both plain OWNED and OWNED+FALLBACK (any key with an
// owned buffer) -- a real write always takes precedence over the cold-start
// seed. The copy, ts_ms stamp, and dirty-invalidation happen atomically
// under the entry lock.
// Returns BB_ERR_INVALID_ARG if req, req->key, or req->snap is NULL.
// Returns BB_ERR_NOT_FOUND if the key is not registered.
bb_err_t bb_cache_update(const bb_cache_update_t *req);

// Delete a registered key, freeing its owned buffer and memoized serialized
// bytes and marking the slot reusable by a future bb_cache_register() call
// (a deleted slot is indistinguishable from a never-used slot).
//
// Returns BB_ERR_NOT_FOUND if the key is not registered.
// Returns BB_ERR_INVALID_ARG if key is NULL.
//
// KNOWN PHASE-A LIMITATION (B1-592): bb_cache has no coupling to bb_event's
// topic lifecycle. Deleting a key registered with BB_CACHE_FLAG_SSE frees the
// bb_cache_entry_t but does NOT unregister the underlying bb_event_topic_t --
// bb_event has no topic-unregister primitive today. The event topic handle is
// leaked for the remaining process life (a subsequent bb_cache_register() of
// the same key registers a NEW event topic and overwrites the field; the old
// topic handle is simply abandoned, not freed). This is acceptable for the
// current callers (e.g. an ingress router evicting a stale bb_sub key) but is
// a real leak for any consumer that deletes+re-registers SSE-flagged keys in
// a hot loop. A bb_event topic-unregister primitive is tracked as a follow-up.
bb_err_t bb_cache_delete(const char *key);

// Returns true if key is currently registered, false otherwise (including
// when bb_cache has not yet been initialized).
bool bb_cache_exists(const char *key);

// Serialize the cached struct into a fresh bb_json obj, wrap it in the
// envelope ({"ts_ms":N,"data":{...}} — see the header-level comment above),
// and post the resulting JSON via bb_event_post to the registered event
// topic. Does NOT touch any ring — ring attachment is the caller's
// responsibility.
// Returns BB_ERR_NOT_FOUND if the key is not registered.
// Returns BB_ERR_INVALID_STATE if the key has no SSE event topic
// (registered with BB_CACHE_FLAG_NONE).
bb_err_t bb_cache_post(const char *key);

// Serialize the cached struct's raw fields into a caller-supplied bb_json
// obj — NOT enveloped (no ts_ms/data wrapper). Use this to embed a key as a
// section of a larger composed document (e.g. /api/health aggregating
// multiple keys); use bb_cache_get_serialized/bb_cache_post for the
// standalone wire contract.
// Returns BB_ERR_NOT_FOUND if the key is not registered.
bb_err_t bb_cache_serialize_into(const char *key, bb_json_t obj);

// Post a pre-serialized payload to the key's SSE event channel.
// Use this when the caller has ALREADY serialized the snapshot (e.g. the
// bb_pub sampler serializes once for both SSE and sinks).  Avoids the
// extra serialize call that bb_cache_post would perform.
// Returns BB_ERR_INVALID_STATE when the key has no SSE event topic.
// Returns BB_ERR_NOT_FOUND if the key is not registered.
bb_err_t bb_cache_post_serialized(const char *key, const char *json, size_t json_len);

// Memoized serialization — the core of serialize-once, COPY-OUT under the lock.
//
// Copies the key's last serialized JSON, WRAPPED in the envelope
// ({"ts_ms":N,"data":{...}} — see the header-level comment above), into the
// caller's buffer. The serializer runs AT MOST ONCE per bb_cache_update()
// generation and its output ("data") is memoized; the envelope's ts_ms is
// applied around the memoized bytes on every call, so owned-mode reads stay
// byte-identical between updates (ts_ms frozen) while getter-mode reads pick
// up a fresh ts_ms each call. Subsequent readers (SSE post, every sink, REST
// polls) get a COPY of the same wrapped bytes without re-serializing "data".
//
// UAF-safe by construction: the copy happens under the entry lock, and the
// caller only ever touches its own buffer. A concurrent bb_cache_update() +
// re-serialize (which frees the entry's internal buffer) can never corrupt an
// in-flight reader, because no caller holds the cache-owned pointer past the
// lock. Size the buffer to the cache's max payload (e.g. the bb_pub worker
// uses CONFIG_BB_PUB_BUFFER_MAX_PAYLOAD_BYTES); REST handlers use a comparable
// stack/heap buffer.
//
// For getter-mode entries (registered with a non-NULL snapshot getter), the
// cache has no dirty signal, so the serializer runs on every call (the data
// may change underneath without an update). Owned-mode entries (snapshot==NULL)
// get true memoization via the dirty flag.
//
// Use bb_cache_serialize_into instead when EMBEDDING a key as a section in a
// larger composed document (e.g. /api/health aggregating multiple keys).
//
//   key      — registered key.
//   buf      — caller-owned destination buffer (must be non-NULL).
//   cap      — capacity of buf in bytes (must include room for the NUL).
//   out_len  — optional; receives strlen of the copied JSON (excludes NUL).
//
// Returns BB_ERR_NOT_FOUND if the key is not registered.
// Returns BB_ERR_INVALID_STATE if no snapshot is available yet.
// Returns BB_ERR_NO_SPACE on serialize allocation failure OR if cap is too
// small to hold the serialized JSON plus its NUL terminator (buf untouched).
bb_err_t bb_cache_get_serialized(const char *key, char *buf, size_t cap, size_t *out_len);

// ---------------------------------------------------------------------------
// state_version (B1-767 PR-3) vs generation -- do not conflate
// ---------------------------------------------------------------------------
//
// state_version is a per-key, monotonically increasing counter that bumps
// exactly once per successful OWNED-mode bb_cache_update() write (see that
// fn's doc comment) -- it counts VALUE WRITES. It resets to 0 on a fresh
// slot occupancy (first register, or register after delete/age-out); an
// idempotent re-register of a still-present (live) key returns early (see
// bb_cache_register()'s idempotency note) and is a no-op -- it does NOT
// reset state_version. Getter-mode keys (no owned buffer) and a
// registered-but-never-written owned key both read 0.
//
// This is DELIBERATELY separate from the existing `generation` counter
// (internal, see bb_cache_espidf.c), which tracks slot IDENTITY -- it bumps
// on every free<->in-use transition of a REGISTRY SLOT (a fresh register or
// a delete), regardless of how many or how few writes land while the slot
// is occupied. Two entirely different axes: `generation` answers "is this
// still the same incarnation of this key I looked up earlier?";
// `state_version` answers "has this key's VALUE changed since I last looked
// at it?". Neither can be derived from the other.
//
// Wrap-safe comparison: state_version is a uint32_t that wraps at 2^32.
// Consumers diffing two captured versions (e.g. bb_cache_serialize's
// "has this changed since I last read it?" check) must compare with
// wrap-safe arithmetic (e.g. `(int32_t)(a - b) > 0`) or plain equality --
// never a naive `>` -- so a wrap does not misreport a newer version as
// older.

// Immutable, walk-safe snapshot of a key's owned value + its state_version.
// `state` points into the CALLER's buffer (copy-under-lock), so the caller
// may walk/serialize it LOCK-FREE -- a torn read is impossible.
typedef struct {
    const void *state;    // -> caller's buf
    size_t      size;     // bytes copied (== registered snap_size)
    uint32_t    version;  // state_version captured atomically with the copy
} bb_cache_snapshot_t;

// Atomically copy the owned struct into buf + capture state_version.
// Owned / owned+fallback only (seeds cold-start on first call, like
// bb_cache_get_raw).
//
// Returns BB_ERR_NOT_FOUND if the key is not registered.
// Returns BB_ERR_INVALID_STATE for getter-mode keys (no owned struct --
// mirrors bb_cache_get_raw).
// Returns BB_ERR_NO_SPACE if cap < the registered snap_size (buf untouched).
// Returns BB_ERR_INVALID_ARG on null args.
bb_err_t bb_cache_snapshot(const char *key, void *buf, size_t cap, bb_cache_snapshot_t *out);

// Non-destructive read of a key's monotonic state_version (no evict, like
// bb_cache_is_stale). 0 means registered-but-never-written, or getter-mode.
//
// Returns BB_ERR_NOT_FOUND if the key is not registered (out_version
// untouched).
// Returns BB_ERR_INVALID_ARG on a null key or out_version.
bb_err_t bb_cache_state_version(const char *key, uint32_t *out_version);

// ---------------------------------------------------------------------------
// Keyed enumeration + compact struct-read accessor
// ---------------------------------------------------------------------------

// Number of currently registered (non-NULL key) entries in the registry.
size_t bb_cache_count(void);

// Look up the key at a raw registry slot index, COPYING the key by value into
// buf under the registry lock -- never returns a raw pointer into the
// registry (same UAF class bb_cache_foreach's snapshot-by-value fix
// eliminated: a concurrent delete+re-register could rename a slot's key
// bytes out from under a caller holding a raw pointer).
//   index — raw slot index, [0, BB_CACHE_MAX_TOPICS).
//   buf   — caller-owned destination buffer (must be non-NULL).
//   cap   — capacity of buf in bytes (must be non-zero).
// A free slot copies "" (empty string) into buf and returns BB_OK.
// Returns BB_ERR_INVALID_ARG on a NULL buf or cap == 0.
// Returns BB_ERR_NOT_FOUND if index >= BB_CACHE_MAX_TOPICS.
// Returns BB_ERR_NO_SPACE if cap is too small to hold the key plus its NUL
// terminator (buf untouched).
bb_err_t bb_cache_key_at(size_t index, char *buf, size_t cap);

// Invoke cb once per registered key. The key set is snapshotted BY VALUE
// (each key's bytes are copied into a stack buffer) under the registry lock,
// which is released before invoking cb -- so cb may safely call bb_cache_*
// (lock not held during cb), including bb_cache_delete/bb_cache_register on
// keys other than the one currently being visited. The by-value copy costs
// BB_CACHE_MAX_TOPICS * BB_CACHE_KEY_MAX stack bytes (default 32*96 = 3 KB)
// but is required now that keys are not add-only: entries can be deleted and
// slots reused (bb_cache_delete), so a snapshot of raw key POINTERS into
// s_entries[] could be renamed out from under an in-flight cb by a concurrent
// delete+re-register racing the lock-released window.
bb_err_t bb_cache_foreach(void (*cb)(const char *key, void *ctx), void *ctx);

// Compact read of an owned-mode key's raw struct bytes.
//   buf — caller-owned destination buffer (must be non-NULL).
//   cap — capacity of buf in bytes (must be non-zero).
// Copies the full owned struct into buf; refuses and does NOT copy if
// cap < key's registered size. Parity with bb_cache_get_serialized:
// refuses rather than truncates on undersized buffer. Owned+fallback keys
// (snapshot != NULL, snap_size > 0) succeed and trigger the cold-start
// snapshot() seed on first call, same as the JSON read paths.
// Returns BB_ERR_NOT_FOUND if the key is not registered.
// Returns BB_ERR_INVALID_STATE for getter-mode keys (no owned struct).
// Returns BB_ERR_INVALID_ARG on null args or cap == 0.
// Returns BB_ERR_NO_SPACE if cap < the stored struct size (buf untouched).
bb_err_t bb_cache_get_raw(const char *key, void *buf, size_t cap);

// ---------------------------------------------------------------------------
// Age-out eviction (B1-592 A3)
// ---------------------------------------------------------------------------

// Report whether a registered AGE_OUT key's current value is stale (age >=
// stale_age_ms) WITHOUT evicting it -- lets a caller degrade a read (e.g.
// mark a REST/SSE payload "stale":true) without paying an eviction+refetch.
// PINNED-policy keys and keys with stale_age_ms == 0 (no stale window)
// always report false while present.
// Returns BB_ERR_NOT_FOUND if the key is not registered (including
// immediately after a LAZY/SWEEP eviction).
bb_err_t bb_cache_is_stale(const char *key, bool *out_stale);

// Age-out sweep backstop lifecycle (B1-592 lifecycle follow-up): the
// periodic background pass over every AGE_OUT-policy key (bb_cache_foreach
// under the hood) that evicts entries past evict_age_ms even when nothing
// reads them (LAZY eviction only fires on a read) is started via the
// pre_http registry hook below when CONFIG_BB_CACHE_SWEEP_ENABLE=y (Kconfig,
// default n) -- mirroring bb_pub_start(). There is no public start call:
// the Kconfig knob is the only control (registration-gated, container-
// invoked). See platform/espidf/bb_cache/bb_cache_espidf.c.

#if BB_CACHE_SWEEP_ENABLE
// Registry hook — starts the age-out eviction sweep backstop. Compiled in
// only when CONFIG_BB_CACHE_SWEEP_ENABLE=y (feature toggle, not registration
// glue -- KEPT; the whole function only exists under this gate).
// bbtool:init tier=pre_http fn=bb_cache_evict_start
bb_err_t bb_cache_evict_start(void);
#else
// No-op stub when the sweep backstop is compiled out (default) -- codegen's
// `// bbtool:init` marker scan has no preprocessor awareness (grep-time,
// see wire_parse.py), so bb_app_init.c unconditionally calls this fn;
// mirrors the bb_alert.h Kconfig-bridge stub pattern.
static inline bb_err_t bb_cache_evict_start(void) { return BB_OK; }
#endif

#ifdef __cplusplus
}
#endif
