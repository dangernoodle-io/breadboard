#pragma once
// Private interface for bb_data_http_common.c and its host test file. Not
// for external consumers -- kept out of include/.
#include "bb_data_http.h"
#include "bb_http.h"
#include "bb_queue.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// STATE (poll) bookkeeping, split out of bb_data_http_client_t (B1-1447,
// epic B1-1123) into its own pool-allocated block -- see
// bb_data_http_common.c's poll pool. dirty_mask / seen_gen are indexed by
// ATTACH-TABLE index (0..BB_DATA_HTTP_MAX_ATTACH-1), NOT by bb_data binding
// index -- this component never sees bb_data binding indices, only
// attach-table slots it owns itself.
typedef struct {
    uint32_t dirty_mask;
    uint32_t seen_gen[BB_DATA_HTTP_MAX_ATTACH];
} bb_data_http_poll_state_t;

// EVENT (push) bookkeeping, split out of bb_data_http_client_t (B1-1447,
// epic B1-1123) into its own pool-allocated block -- see
// bb_data_http_common.c's push pool.
//
// cursor (B1-1033 PR-3, KB 1443/1444) is this client's read position into
// the shared EVENT ring (see bb_data_http_common.c's s_event_ring): the next
// global push-sequence number this client has not yet drained. Set to the
// ring's current total-pushed count on acquire -- a freshly-attached client
// only receives EVENTs pushed AFTER it connects, never ring backlog (mirrors
// an SSE "new events only" subscription rather than replay-on-connect;
// STATE's fresh-render-on-connect semantics do not apply to EVENT).
//
// dropped / drop_marker_pending implement the backpressure contract (see
// bb_data_http_sweep_step()'s EVENT drain doc): when this client's own
// `outbound` queue has no room for a drained event, the event is dropped
// (not evicted from the shared ring -- other clients are unaffected) and
// dropped increments. A "dropped:N" marker frame is queued for this client
// at the next opportunity outbound has room, then drop_marker_pending
// clears; dropped itself is cumulative and never resets
// (bb_data_http_client_dropped_count()).
typedef struct {
    uint32_t cursor;
    uint32_t dropped;
    bool     drop_marker_pending;
} bb_data_http_push_state_t;

// One fd-table client slot (B1-1033 Option A, KB 1443). Indexed 0..N-1 by
// slot position within the static pool -- see bb_data_http_common.c.
//
// poll (B1-1447): STATE bookkeeping, pool-allocated at acquire time only
// when `subscribe_mask` includes BB_DATA_HTTP_SUBSCRIBE_STATE -- NULL
// otherwise (an EVENT-only client never touches it; every call site that
// would dereference it gates on `subscribe_mask` or `poll != NULL` FIRST --
// see bb_data_http_client_acquire()'s force-dirty walk and
// bb_data_http_sweep_step()'s detect/drain phases). A client whose kind the
// core cannot back with a pool slot (pool exhausted) never comes into
// existence -- bb_data_http_client_acquire() fails BB_ERR_NO_SPACE instead
// of returning a client with a NULL poll a caller might not expect (see
// bb_data_http_client_acquire()'s doc, bb_data_http.h).
//
// push (B1-1447): EVENT bookkeeping, pool-allocated at acquire time only
// when `subscribe_mask` includes BB_DATA_HTTP_SUBSCRIBE_EVENT -- NULL
// otherwise, same non-participation discipline as poll above (see
// bb_data_http_sweep_step()'s drain phase, which gates
// drain_client_events() on `push != NULL`).
//
// outbound_max_bytes mirrors the byte budget passed to bb_queue_create()
// for `outbound` at acquire time. bb_queue exposes no "would this push fit"
// query, so the EVENT drain path (which must NOT rely on outbound's
// BB_QUEUE_EVICT_OLDEST auto-eviction -- see the drop-not-evict rationale
// above) keeps its own copy to pre-check room before pushing.
//
// send_fail_count (B1-1424/B1-1429) is the bounded send-retry contract's
// per-client consecutive-failure counter -- see bb_data_http_sweep_step()'s
// flush-contract doc (bb_data_http.h) for the full design. Reset to 0 on
// every successful send_fn call (and on acquire); incremented on every
// RETRIABLE (BB_ERR_TIMEOUT) failure only -- a FATAL failure tears the
// client down immediately instead of touching this counter. It is a 4-byte
// cost per client slot (CONFIG_BB_DATA_HTTP_MAX_CLIENTS * 4 bytes total BSS,
// e.g. 8 bytes at the default cap of 2) -- deliberately per-CLIENT, not
// per-frame: bb_queue's entries carry no spare per-entry field to hang a
// counter off of, and a wedged transport (the failure mode this bounds)
// fails every frame it is asked to send, not just one particular frame, so
// a single client-wide counter observes the same signal a per-frame counter
// would, for a quarter the state and no bb_queue changes.
//
// send_fn/send_ctx/abort_fn/abort_ctx (B1-1123 PR-1) are the PER-CONSUMER
// seam override -- see bb_data_http.h's PER-CONSUMER SEAMS doc for the full
// rationale. NULL (the zero-default -- every existing HTTP call site leaves
// these unset) means "fall back to the module-wide default installed via
// bb_data_http_set_send_fn()/bb_data_http_set_abort_fn()"; the flush loop
// (bb_data_http_common.c) resolves `c->send_fn ? c->send_fn : s_send_fn`
// (and likewise for abort_fn) on every call. Written once by the acquiring
// task at acquire time, then read-only for the rest of this client's
// lifetime by the single task that owns bb_data_http_sweep_step() -- same
// discipline as every other field below except `in_use`/`pending_release`
// (see the TASK OWNERSHIP doc immediately below for those two).
//
// TASK OWNERSHIP (B1-1424 HIGH fix, deferred reap; rewritten B1-1482 review
// MEDIUM+LOW fix to also cover `in_use`'s own cross-task read/write map,
// which the earlier version of this doc did not -- it asserted an
// ownership model that a since-added lock, and `in_use`'s own conversion to
// atomic_bool, straddle; the tiers below are the corrected, complete
// picture, not a re-assertion of the old one). Three tiers, by field:
//
//   1. SINGLE-TASK-OWNED (every field below except `in_use` and
//      `pending_release`): created by whichever task calls
//      bb_data_http_client_acquire() (fully populated BEFORE `in_use`
//      publishes, below), then EXCLUSIVELY owned and mutated by the single
//      task that calls bb_data_http_sweep_step() (the espidf backend's
//      broadcaster task; the test-runner thread on host) for the rest of
//      the client's lifetime, INCLUDING the eventual
//      bb_data_http_client_release() call itself -- that function is NOT
//      cross-task-safe and must only ever be called from that one owning
//      task (see its doc, bb_data_http.h). Plain (non-atomic) types, no
//      lock -- a foreign task never touches these directly.
//
//   2. ATOMIC (`in_use`, `pending_release`): each independently safe for
//      concurrent cross-task access via C11 atomics, WITHOUT s_clients_lock
//      (bb_data_http_common.c) -- neither needs it, and reads/writes of
//      either never take it:
//        - `pending_release`: any task may SET it (via
//          bb_data_http_client_request_release()) to ask the owning task to
//          release this client on its next bb_data_http_sweep_step() call,
//          but only the owning task ever READS or CLEARS it
//          (bb_data_http_client_release() resets it as part of a normal
//          release; bb_data_http_client_acquire() resets it again on
//          reuse). A real hazard this closes: the espidf backend's WS
//          disconnect callback fires on bb_ws_server's own worker task, not
//          the broadcaster -- calling bb_data_http_client_release() directly
//          from there (destroying `outbound`, clearing `in_use`) could race
//          the broadcaster's own in-flight read/write of this SAME client
//          inside bb_data_http_sweep_step() on another core.
//        - `in_use` (B1-1482 review fix, see below): read by
//          bb_data_http_client_acquire() (the acquiring task, under
//          s_clients_lock -- the LOCK here protects a DIFFERENT invariant,
//          the blocking-budget/free-slot scan-then-claim atomicity, not
//          `in_use`'s own read safety, which the atomic type already
//          provides) and by every reader inside bb_data_http_sweep_step()/
//          bb_data_http_active_client_count() (the owning task, always
//          unlocked); written true by bb_data_http_client_acquire() (the
//          acquiring task, under s_clients_lock, paired with the
//          reservation clear -- see s_clients_lock's own doc,
//          bb_data_http_common.c) and false by
//          bb_data_http_client_release() (the owning task, deliberately
//          UNLOCKED -- see that function's own doc for why it must never
//          take s_clients_lock).
//
// s_clients_lock (bb_data_http_common.c) therefore has a narrow, specific
// job: it makes bb_data_http_client_acquire()'s OWN scan-then-claim
// sequence atomic against a SECOND CONCURRENT acquire() call (the B1-1482
// review TOCTOU fix) -- it is never taken by bb_data_http_client_release(),
// bb_data_http_sweep_step(), or bb_data_http_active_client_count(), and it
// does NOT extend this struct's single-task-ownership model for tier 1
// fields, nor does it substitute for `in_use`/`pending_release`'s own
// atomicity in tier 2 -- those two properties (the lock's acquire-side
// mutual exclusion, and `in_use`'s atomic cross-task read/write safety)
// are independent and both required: the lock alone would not make
// release()'s unlocked write of `in_use` safe against acquire()'s locked
// reads of it (that needs the atomic type); the atomic type alone would
// not make two concurrent acquire() calls' scan-then-claim sequences
// mutually exclusive (that needs the lock).
//
// SHARED EVENT RING -- a SEPARATE, real exception (B1-1450, epic B1-1123),
// not part of the per-client single-task-ownership model above: the
// module-level EVENT ring (bb_data_http_common.c's s_event_ring,
// s_event_total_pushed, s_event_last_gen -- NOT any field on this struct)
// is now genuinely multi-writer-safe, guarded by a short portMUX critical
// section (s_event_ring_mux) wrapped around a CLAIM/RENDER/COMMIT-OR-
// REVOKE protocol, not merely around the final push -- the generation
// compare-and-advance that decides whether a key needs a fresh render is
// itself claimed under the lock, before render_fn runs, so only one caller
// ever wins the right to render+push a given (key, generation) pair; a
// failed render revokes the claim via a locked compare-and-restore unless
// a second caller has since claimed a newer generation. See
// bb_data_http_push_pump()'s doc (bb_data_http.h) for the exact per-key
// sequence and why locking only the push (without the decision) would
// still leave a double-delivery hazard open. This lets a second,
// independent producer task call bb_data_http_push_pump() directly,
// concurrently with the broadcaster task's own sweep_step()-driven call.
// It does NOT change anything about this struct or the per-client
// ownership rules above: draining the ring into any client's `outbound`
// (push->cursor, dropped, drop_marker_pending, outbound itself) is still
// exclusively read/written by the single owning task inside
// bb_data_http_sweep_step(), unlocked, exactly as before -- only the
// ring's own module-level statics gained the claim/commit/revoke lock, not
// the per-client drain/flush path.
struct bb_data_http_client {
    // B1-1482 review MEDIUM+LOW fix: atomic_bool, not a plain bool -- see the
    // TASK OWNERSHIP doc above for the full cross-task read/write map. This
    // is now the SECOND field in this struct requiring atomic access
    // (mirrors `pending_release` below): bb_data_http_client_acquire() reads
    // it under s_clients_lock (bb_data_http_common.c) from the acquiring
    // task, while bb_data_http_client_release() writes it UNLOCKED from the
    // sweep-owning task -- a plain bool would be a genuine cross-task data
    // race on those two sides (the lock only ever covers acquire()'s OWN
    // reads/writes, never release()'s, by design -- see the TASK OWNERSHIP
    // doc's own reasoning for why release() deliberately stays lock-free).
    // atomic_bool closes that without extending s_clients_lock's reach into
    // the sweep task's territory. Same size/alignment as plain `bool` on
    // every ABI this struct is compiled under (bool is natively
    // lock-free-atomic on both the LP64 host toolchain and the ILP32
    // xtensa-esp32-elf-gcc cross toolchain) -- verified by the pinned
    // sizeof() assert below, unchanged.
    atomic_bool                     in_use;
    // B1-1452: placed here, immediately after `in_use`, rather than
    // appended after the pointer-heavy tail below -- both are single-byte
    // (in_use is atomic_bool as of B1-1482, still 1 byte -- see its own doc
    // above), so grouping them lands warned_no_send_fn in alignment padding
    // that already existed ahead of topic_filter on BOTH ABIs this struct
    // is compiled under (ILP32 and LP64), instead of costing a whole extra
    // 4-byte-aligned word on ILP32 (which a tail append would have -- see
    // the pinned-sizeof assert below). Measured, not assumed: verified by
    // direct sizeof() probe on both widths after this placement.
    //
    // Set the first time the flush loop resolves BOTH send seams
    // (per-client send_fn AND the module-wide s_send_fn fallback) to NULL
    // for this client AND finds queued frames it can never flush -- see
    // bb_data_http_sweep_step()'s flush-loop doc. Gates the one-shot WARN
    // there so a client stuck with no usable send seam announces itself
    // exactly once, never once-per-sweep-per-client. Reset to false on
    // every bb_data_http_client_acquire() (fresh slot reuse must not
    // inherit a prior occupant's warned state).
    bool                            warned_no_send_fn;
    // B1-1482: resolved copy of cfg->non_blocking at acquire time (see
    // bb_data_http_client_cfg_t's own doc, bb_data_http.h, for the
    // zero-default polarity rationale) -- what bb_data_http_client_acquire()'s
    // blocking-budget scan actually counts. Grouped with the other
    // single-byte bools ahead of `topic_filter` for the same reason
    // `warned_no_send_fn` is (see the pinned-sizeof assert below): it lands
    // in alignment padding that already existed ahead of `topic_filter` on
    // both ABIs this struct is compiled under, so the footprint is
    // unaffected -- verified by the same sizeof() probe.
    bool                            non_blocking;
    char                            topic_filter[BB_DATA_HTTP_TOPIC_MAX];  // "" == all attached keys
    uint32_t                        subscribe_mask;  // resolved kind bitmask; see bb_data_http_client_cfg_t's doc (bb_data_http.h)
    bb_data_http_poll_state_t      *poll;  // NULL unless subscribe_mask includes STATE -- see its own doc above
    bb_data_http_push_state_t      *push;  // NULL unless subscribe_mask includes EVENT -- see its own doc above
    bb_queue_t                      outbound;
    size_t                          outbound_max_bytes;
    uint32_t                        send_fail_count;
    atomic_bool                     pending_release;
    bb_data_http_send_fn            send_fn;   // NULL -> module-wide default (s_send_fn)
    void                           *send_ctx;
    bb_data_http_abort_fn           abort_fn;  // NULL -> module-wide default (s_abort_fn)
    void                           *abort_ctx;
    bb_data_http_client_lifetime_t  lifetime;  // resolved at acquire; see bb_data_http_client_cfg_t's doc (bb_data_http.h)
};

// Pinned shrink proof (B1-1447, re-pinned B1-1448, re-pinned B1-1465/B1-1466
// for the `lifetime` field, re-pinned again below for `warned_no_send_fn`):
// splitting STATE/EVENT bookkeeping into pool-allocated poll/push pointers,
// and later dropping the last HTTP-specific fd/is_ws fields (B1-1448, epic
// B1-1123), must actually SHRINK this struct, not just rename fields around
// the same footprint -- see bb_serialize_json_tok.c for this repo's
// identical pinned-sizeof convention. A future field added here (or a
// future revert) that changes this size needs a deliberate, reviewed edit
// to this assert, not a silent drift. Pointer-width-dependent (this struct
// carries several pointer-sized fields): 120 bytes on a 64-bit host build
// (native test envs), 84 bytes on a 32-bit target (ESP32/xtensa ILP32) --
// both pinned here rather than assuming one ABI, since this header is
// compiled by both. `warned_no_send_fn` (B1-1452) is a single-byte `bool`
// placed immediately after `in_use` (see the struct's own field comment)
// rather than appended after the pointer-heavy tail: on LP64 it lands in
// alignment padding that already existed ahead of `topic_filter`, so the
// footprint is UNCHANGED (still 120); on ILP32 that same padding exists too
// (the field was NOT free there when appended at the tail -- an append grew
// ILP32 84 -> 88 -- but IS free here), so ILP32 stays at 84, unchanged from
// before this field existed. Both values verified by direct sizeof() probe:
// LP64 via a native gcc-16 host compile, ILP32 via the real
// xtensa-esp32-elf-gcc cross toolchain (not `-m32`, which this host's
// arm64 gcc cannot target) -- and ILP32 is additionally load-bearing on a
// green `make smoke-esp32`, a real xtensa build where this
// `_Static_assert` is compile-time-enforced. PRECONDITION: this two-arm
// ternary only distinguishes ILP32 (4-byte pointers, e.g. ESP32/xtensa)
// from LP64 (8-byte pointers, e.g. native host); it does NOT cover a
// 16-bit-pointer target (e.g. an AVR/Arduino backend, which this workspace
// does have as a family, though bb_data_http itself is realistically
// ESP32/host-only) -- such a build would spuriously trip this assert and
// need a third arm added, not silently pass under either existing one.
//
// B1-1482 review MEDIUM+LOW fix: `in_use` converted from `bool` to
// `atomic_bool` (see the struct's own field doc above) -- re-verified
// zero-cost on both ABIs the same way as every field re-pin above: `bool`
// is natively lock-free-atomic on both the LP64 host toolchain and the
// ILP32 xtensa-esp32-elf-gcc cross toolchain, so `_Atomic(bool)` carries
// the identical 1-byte size/alignment as plain `bool` on both -- exactly
// the same property `pending_release` (already `atomic_bool`, further down
// this struct) has always relied on. This is NOT assumed by analogy alone:
// re-verified directly, both arms, same two builds as above (`make test`
// LP64, `make smoke-esp32` ILP32) -- both still hit exactly 120/84.
_Static_assert(sizeof(struct bb_data_http_client) == (sizeof(void *) == 8 ? 120 : 84),
               "bb_data_http_client_t size changed -- update this pin (B1-1452 warned_no_send_fn field, moved next to in_use to stay in existing ILP32 padding; B1-1482 non_blocking field, grouped the same way, verified to cost zero additional bytes on both ABIs; B1-1482 review fix -- in_use converted bool -> atomic_bool, verified to also cost zero additional bytes on both ABIs, same as pending_release's own pre-existing atomic_bool -- bool is natively lock-free-atomic on every ABI this struct compiles under, both re-verified by direct sizeof() probe: LP64 via `make test`, ILP32 via a real `make smoke-esp32` xtensa-esp32-elf-gcc build, this assert compile-time-enforced in both)");

#ifdef BB_DATA_HTTP_TESTING
// Test accessors -- expose fd-table/attach-table internals without widening
// the public API surface.

// Returns the dirty-mask bitset currently set on client `c`. Returns 0 if
// `c` is NULL.
uint32_t bb_data_http_client_dirty_mask_for_test(const bb_data_http_client_t *c);

// Returns client `c`'s recorded state_seen_gen for attach index `idx`.
// Returns 0 if `c` is NULL or idx is out of range.
uint32_t bb_data_http_client_seen_gen_for_test(const bb_data_http_client_t *c, size_t idx);

// Returns the number of entries currently queued in client `c`'s outbound
// bb_queue. Returns 0 if `c` is NULL.
size_t bb_data_http_client_outbound_count_for_test(const bb_data_http_client_t *c);

// Returns client `c`'s current consecutive RETRIABLE send_fn failure count
// (see bb_data_http_client_t's send_fail_count doc). Returns 0 if `c` is
// NULL.
uint32_t bb_data_http_client_send_fail_count_for_test(const bb_data_http_client_t *c);

// Returns client `c`'s current event_cursor (next undrained EVENT global
// sequence number). Returns 0 if `c` is NULL.
uint32_t bb_data_http_client_event_cursor_for_test(const bb_data_http_client_t *c);

// Returns client `c`'s current pending_release flag (see
// bb_data_http_client_t's TASK OWNERSHIP doc). Returns false if `c` is
// NULL.
bool bb_data_http_client_pending_release_for_test(const bb_data_http_client_t *c);

// B1-1447: direct proof that a client whose subscribe_mask excludes STATE
// (or the STATE poll pool was exhausted -- see
// bb_data_http_client_acquire()'s doc, bb_data_http.h) carries a genuinely
// NULL `poll` pointer, rather than inferring it indirectly from every
// poll-reading accessor's own NULL-safe zero-return. Returns true if `c` is
// NULL (mirrors "no client, nothing to point at").
bool bb_data_http_client_poll_is_null_for_test(const bb_data_http_client_t *c);

// Mirror of bb_data_http_client_poll_is_null_for_test() for `push`. Returns
// true if `c` is NULL.
bool bb_data_http_client_push_is_null_for_test(const bb_data_http_client_t *c);

// B1-1452: current value of client `c`'s warned_no_send_fn flag (see
// bb_data_http_client_t's own doc) -- lets a test assert the flush loop's
// one-shot no-send-seam WARN fired exactly once. Returns false if `c` is
// NULL.
bool bb_data_http_client_warned_no_send_fn_for_test(const bb_data_http_client_t *c);

// B1-1449: current occupancy of the shared EVENT ring -- lets a test assert
// bb_data_http_push_pump() fed the ring directly, independent of any
// client's own drained cursor. Returns 0 if the ring has not been created
// (bb_data_http_init() not yet called, or bb_data_http_reset_for_test()
// since).
size_t bb_data_http_event_ring_count_for_test(void);

// B1-1450: current value of the module-level, per-attach-index
// s_event_last_gen for `key` -- lets a test observe the claim/commit/
// revoke protocol's outcome directly. Returns 0 if `key` is not attached.
uint32_t bb_data_http_event_last_gen_for_test(const char *key);

// B1-1450: directly overwrite s_event_last_gen for `key`'s attach index,
// bypassing the claim/commit/revoke protocol -- lets a host test simulate
// a second, concurrent bb_data_http_push_pump() caller claiming a NEWER
// generation for the SAME key while THIS caller's own render_fn call is
// still in flight (the compare-and-restore revoke's "leave it alone"
// branch, see bb_data_http_push_pump()'s own doc, bb_data_http.h), which
// the single-threaded host harness cannot otherwise reach. No-op if `key`
// is not attached.
void bb_data_http_event_last_gen_set_for_test(const char *key, uint32_t gen);
#endif

// GET /api/events route descriptor (B1-1215): schema-only (.handler == NULL)
// so the same static bb_route_t is shared verbatim between the ESP-IDF
// composition helper (bb_data_http_espidf_routes_init(), which pairs it with
// the real handler via bb_http_register_route_descriptor_only()) and host
// tests -- one descriptor, no mirror. .handler stays NULL because this
// descriptor lives in the portable bb_data_http_common.c, which host tests
// compile directly, while the real handler (events_get_handler) lives in an
// ESP-IDF-only platform TU; setting .handler here would leak a platform-only
// symbol into a portable file, so the wiring is split between this
// descriptor and bb_data_http_espidf_routes_init()'s separate handler
// registration. content_type is "text/event-stream" with schema == NULL: a
// multiplexed stream over every bb_data-bound topic has no single response
// schema; bb_openapi's oneOf synthesis (build_sse_oneof_fragment()) fills
// the content block from the registered SSE topic schemas instead.
const bb_route_t *bb_data_http_events_route(void);

#ifdef __cplusplus
}
#endif
