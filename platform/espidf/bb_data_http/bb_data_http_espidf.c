// ESP-IDF backend for bb_data_http (B1-1033 first bench-flash de-risk,
// design KB 1447): wires the pure core's three injected seams to real
// bb_data/socket calls and drives bb_data_http_sweep_step() from ONE
// process-lifetime broadcaster task -- never a per-client task (the
// B1-484/B1-492 static TCB-reuse hazard bb_event_routes/bb_sse_writer must
// guard against does not apply here, because no task is ever created or
// torn down per connection).
//
// httpd async-req model: the SYNC route handler
// (bb_data_http_espidf_client_connect) does socket hardening + sends SSE
// response headers + begins the async handler + acquires a bb_data_http
// client slot + records the fd -> async_req mapping in this file's own
// side table, then returns immediately. No per-connection task, no
// blocking loop on the httpd task. All draining (render + send) happens
// later, on the broadcaster task's own sweep cadence.
//
// SSE headers are INLINED here, not shared with bb_sse_writer's own
// (near-identical) header block: bb_sse_writer is a per-client-task model
// this component deliberately does not use, and it is deleted wholesale at
// the B1-1045 cutover -- consolidating with code already slated for
// deletion is the wrong direction (KB 1447 fork #2).
#include "bb_data_http.h"
#include "bb_data_http_internal.h"
#include "bb_data.h"
#include "bb_http_server.h"
#include "bb_task.h"
#include "bb_log.h"
#include "bb_ws_server.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/tcp.h>

#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif

// ---------------------------------------------------------------------------
// Config bridge (CONFIG_X -> X, C default -- never shadow the generated
// symbol with a bare #ifndef). See KB 1447: SEND_TIMEOUT_MS=20 (short --
// paid by the one shared broadcaster task per stalled client per sweep;
// MAX_CLIENTS*20ms budget stays well under the 50ms sweep cadence),
// SWEEP_INTERVAL_MS=50, TASK_STACK=4096, TASK_PRIORITY=4 (below lwip),
// RENDER_SCRATCH=256 (fixed stopgap -- fork #1, no per-key snap_size getter
// in this de-risk; bb_data_http_attach_sized() adds the attach-time loud
// guard for a binding that would exceed it, B1-1045 PR-4).
// ---------------------------------------------------------------------------
#ifdef CONFIG_BB_DATA_HTTP_SEND_TIMEOUT_MS
#define BB_DATA_HTTP_SEND_TIMEOUT_MS CONFIG_BB_DATA_HTTP_SEND_TIMEOUT_MS
#endif
#ifndef BB_DATA_HTTP_SEND_TIMEOUT_MS
#define BB_DATA_HTTP_SEND_TIMEOUT_MS 20
#endif

#ifdef CONFIG_BB_DATA_HTTP_RECV_TIMEOUT_MS
#define BB_DATA_HTTP_RECV_TIMEOUT_MS CONFIG_BB_DATA_HTTP_RECV_TIMEOUT_MS
#endif
#ifndef BB_DATA_HTTP_RECV_TIMEOUT_MS
#define BB_DATA_HTTP_RECV_TIMEOUT_MS 30000
#endif

// One-shot widened SO_SNDTIMEO for a client's FIRST SSE frame only (B1-1424
// review item 4, B1-1033's curl -N closure criterion): hardware showed the
// first real send after accept failing (EAGAIN) at ~65ms, well past the
// steady-state BB_DATA_HTTP_SEND_TIMEOUT_MS (20ms) budget -- a cold-socket
// TCP-slow-start cost the connect-time preamble used to pay too (B1-1429,
// now removed), except this cost cannot be removed the same way: it is the
// client's very first REAL frame, which the client (curl -N in particular,
// unlike a browser's EventSource) has no fallback for if it is lost.
// Default 120ms gives ~85% margin over the observed 65ms. Applied for
// exactly ONE send_fn call per client -- see
// bb_data_http_espidf_slot_t's `warmed` field -- then reverts to
// BB_DATA_HTTP_SEND_TIMEOUT_MS unconditionally, so steady-state delivery
// never pays this cost. See s_cold_start_available's doc (this file) for
// the compile-time-argued per-sweep budget impact this is bounded to.
#ifdef CONFIG_BB_DATA_HTTP_FIRST_FRAME_TIMEOUT_MS
#define BB_DATA_HTTP_FIRST_FRAME_TIMEOUT_MS CONFIG_BB_DATA_HTTP_FIRST_FRAME_TIMEOUT_MS
#endif
#ifndef BB_DATA_HTTP_FIRST_FRAME_TIMEOUT_MS
#define BB_DATA_HTTP_FIRST_FRAME_TIMEOUT_MS 120
#endif

#ifdef CONFIG_BB_DATA_HTTP_SWEEP_INTERVAL_MS
#define BB_DATA_HTTP_SWEEP_INTERVAL_MS CONFIG_BB_DATA_HTTP_SWEEP_INTERVAL_MS
#endif
#ifndef BB_DATA_HTTP_SWEEP_INTERVAL_MS
#define BB_DATA_HTTP_SWEEP_INTERVAL_MS 50
#endif

#ifdef CONFIG_BB_DATA_HTTP_TASK_STACK_BYTES
#define BB_DATA_HTTP_TASK_STACK_BYTES CONFIG_BB_DATA_HTTP_TASK_STACK_BYTES
#endif
#ifndef BB_DATA_HTTP_TASK_STACK_BYTES
#define BB_DATA_HTTP_TASK_STACK_BYTES 4096
#endif

#ifdef CONFIG_BB_DATA_HTTP_TASK_PRIORITY
#define BB_DATA_HTTP_TASK_PRIORITY CONFIG_BB_DATA_HTTP_TASK_PRIORITY
#endif
#ifndef BB_DATA_HTTP_TASK_PRIORITY
#define BB_DATA_HTTP_TASK_PRIORITY 4
#endif

#ifdef CONFIG_BB_DATA_HTTP_RENDER_SCRATCH_BYTES
#define BB_DATA_HTTP_RENDER_SCRATCH_BYTES CONFIG_BB_DATA_HTTP_RENDER_SCRATCH_BYTES
#endif
#ifndef BB_DATA_HTTP_RENDER_SCRATCH_BYTES
#define BB_DATA_HTTP_RENDER_SCRATCH_BYTES 256
#endif

// Side table sizing mirrors bb_data_http's own client-slot cap -- one slot
// here per possible bb_data_http_client_t, never independently sized.
#ifdef CONFIG_BB_DATA_HTTP_MAX_CLIENTS
#define BB_DATA_HTTP_ESPIDF_MAX_CLIENTS CONFIG_BB_DATA_HTTP_MAX_CLIENTS
#endif
#ifndef BB_DATA_HTTP_ESPIDF_MAX_CLIENTS
#define BB_DATA_HTTP_ESPIDF_MAX_CLIENTS 2
#endif

// Sweep-budget invariant (B1-1424 review fix -- corrected claim, was
// overstated): MAX_CLIENTS*SEND_TIMEOUT_MS < SWEEP_INTERVAL_MS (checked
// below) bounds ONLY the RETRIABLE-failure sub-case of the flush loop's
// per-client `while` (bb_data_http_common.c) -- a send_fn call that fails
// fast (a timed-out/would-block socket write) costs at most
// SEND_TIMEOUT_MS once per client before that client's flush stops for the
// sweep (bb_data_http_sweep_step()'s flush-contract doc, bb_data_http.h).
//
// It is NOT a general per-sweep worst-case bound: the SAME `while` loop
// keeps calling send_fn for as long as each call SUCCEEDS, so a client
// whose socket is congested but not yet timed-out -- each write completing
// just under SEND_TIMEOUT_MS -- can pay close to SEND_TIMEOUT_MS per frame,
// up to CONFIG_BB_DATA_HTTP_OUTBOUND_CAPACITY frames, in ONE sweep_step()
// call: OUTBOUND_CAPACITY * SEND_TIMEOUT_MS per client (default
// 8*20=160ms), and up to MAX_CLIENTS * OUTBOUND_CAPACITY *
// SEND_TIMEOUT_MS in the worst case across every active client in the same
// call (default 2*8*20=320ms) -- 6.4x the 50ms default sweep interval.
// This is deliberately left unbounded rather than capped: a slow-but-
// succeeding socket completing every write is the PATHOLOGICAL case (a
// congested link, not a stalled one), not the steady state -- an ordinary
// successful send_fn call completes in microseconds, so this bound is a
// theoretical ceiling, not an expected cost. The compile-time check below
// still catches a genuinely bad MAX_CLIENTS/SEND_TIMEOUT_MS/
// SWEEP_INTERVAL_MS combination for the case it DOES bound (default
// config: 2*20=40ms < 50ms sweep, passes).
#if (BB_DATA_HTTP_ESPIDF_MAX_CLIENTS * BB_DATA_HTTP_SEND_TIMEOUT_MS) >= BB_DATA_HTTP_SWEEP_INTERVAL_MS
#error "bb_data_http: MAX_CLIENTS*SEND_TIMEOUT_MS must stay under SWEEP_INTERVAL_MS -- the broadcaster's per-sweep RETRIABLE-failure worst-case block time cannot reach or exceed the sweep cadence (see this check's full doc above for what it does NOT bound)"
#endif

// First-frame cold-start budget (B1-1424 review item 4): deliberately NOT
// covered by the compile-time check above -- it is a separate, ONE-TIME-
// PER-CLIENT cost (see BB_DATA_HTTP_FIRST_FRAME_TIMEOUT_MS's doc), not a
// recurring per-sweep one, so it is not held to the same "never reach the
// sweep cadence" bar. It IS bounded, though, via s_cold_start_available
// (this file's broadcaster_task()/espidf_send_fn): at most ONE client may
// use the widened timeout per sweep_step() call, so the worst-case extra
// cost any single sweep_step() call pays is (MAX_CLIENTS-1) *
// SEND_TIMEOUT_MS [every OTHER active client failing fast, the
// RETRIABLE-failure bound above] + FIRST_FRAME_TIMEOUT_MS [the one
// cold-start client] -- default (2-1)*20 + 120 = 140ms, vs MAX_CLIENTS *
// FIRST_FRAME_TIMEOUT_MS = 2*120 = 240ms if every client could use it in
// the same sweep. 140ms still exceeds the 50ms default
// BB_DATA_HTTP_SWEEP_INTERVAL_MS -- accepted, not hidden: it can only
// happen ONCE per client (bb_data_http_espidf_slot_t's `warmed` field
// latches true after the first attempt, success or fatal-failure alike),
// self-limiting rather than a recurring steady-state cost, unlike the
// bound the compile-time check above actually enforces.

// One SSE frame ("data: <payload>\n\n") must fit the largest single
// rendered value (CONFIG_BB_DATA_HTTP_OUTBOUND_ENTRY_MAX) plus the "data: "
// prefix and "\n\n" suffix -- 16 bytes of slack covers both with room.
#ifdef CONFIG_BB_DATA_HTTP_OUTBOUND_ENTRY_MAX
#define BB_DATA_HTTP_ESPIDF_FRAME_MAX (CONFIG_BB_DATA_HTTP_OUTBOUND_ENTRY_MAX + 16)
#endif
#ifndef BB_DATA_HTTP_ESPIDF_FRAME_MAX
#define BB_DATA_HTTP_ESPIDF_FRAME_MAX (512 + 16)
#endif

static const char *TAG = "bb_data_http_espidf";

// ---------------------------------------------------------------------------
// fd -> async_req side table (B1-1033, KB 1447). Populated by
// bb_data_http_espidf_client_connect() (httpd task), consumed by the
// broadcaster task's send_fn and peer-liveness pre-pass -- two different
// tasks, possibly on different cores (the broadcaster is BB_TASK_CORE_ANY),
// so every read and write of s_slots is protected by s_slots_mux, a
// portMUX spinlock. The lock guards ONLY the table mutation/snapshot itself
// -- callers snapshot the fields they need under the lock, release it, then
// do any blocking I/O (send, peer-alive probe, teardown) outside the lock;
// nothing that can block (socket send, recv) or take non-trivial time may
// ever execute while s_slots_mux is held. in_use is published last on
// alloc and cleared as part of slot_free_locked(), both inside the lock, so
// a reader never observes in_use=true with stale/NULL fd or async_req.
// ---------------------------------------------------------------------------
// `warmed` (B1-1424 review item 4): false until this client's first
// espidf_send_fn call attempt completes (success or failure alike -- see
// espidf_send_fn's SSE branch); gates the one-shot widened first-frame
// SO_SNDTIMEO (BB_DATA_HTTP_FIRST_FRAME_TIMEOUT_MS). Set at connect time
// (bb_data_http_espidf_client_connect()) and read/written ONLY by
// espidf_send_fn on the broadcaster task thereafter -- same set-once-at-
// connect-then-broadcaster-owned pattern as fd/async_req/client below, no
// separate lock needed for it.
typedef struct {
    bool                    in_use;
    int                     fd;
    bb_http_request_t      *async_req;
    bb_data_http_client_t  *client;
    bool                    warmed;
} bb_data_http_espidf_slot_t;

static bb_data_http_espidf_slot_t s_slots[BB_DATA_HTTP_ESPIDF_MAX_CLIENTS];
static portMUX_TYPE s_slots_mux = portMUX_INITIALIZER_UNLOCKED;

// Caller must hold s_slots_mux. B1-1123 PR-1: send_fn now carries the
// bb_data_http_client_t handle rather than an fd, so this side table is
// looked up by that same handle -- it already stores `client` per slot (set
// at connect time, below), so this is a straight pointer-equality scan
// rather than the fd comparison it replaces.
static bb_data_http_espidf_slot_t *slot_find_by_client_locked(const bb_data_http_client_t *client)
{
    for (size_t i = 0; i < BB_DATA_HTTP_ESPIDF_MAX_CLIENTS; i++) {
        if (s_slots[i].in_use && s_slots[i].client == client) {
            return &s_slots[i];
        }
    }
    return NULL;
}

// Caller must hold s_slots_mux.
static bb_data_http_espidf_slot_t *slot_alloc_locked(void)
{
    for (size_t i = 0; i < BB_DATA_HTTP_ESPIDF_MAX_CLIENTS; i++) {
        if (!s_slots[i].in_use) {
            return &s_slots[i];
        }
    }
    return NULL;
}

// Caller must hold s_slots_mux.
static void slot_free_locked(bb_data_http_espidf_slot_t *slot)
{
    slot->in_use   = false;
    slot->fd       = -1;
    slot->async_req = NULL;
    slot->client   = NULL;
    slot->warmed   = false;
}

// ---------------------------------------------------------------------------
// WS fd -> (server, client) side table (B1-1050 PR-1). Same locking
// discipline as s_slots/s_slots_mux above -- a separate table + spinlock
// rather than reusing the SSE one because a WS slot carries no async_req
// (bb_ws_server has no async-handoff concept; sends go through
// bb_ws_server_broadcast_frame_async(server, fd, ...) instead) and the two
// tables are populated/consumed from different callback paths (bb_ws_server's
// global connect/disconnect callbacks vs the SSE route handler + broadcaster
// pre-pass). Sized the same as s_slots -- both draw client slots from the
// same shared bb_data_http_client_t pool (CONFIG_BB_DATA_HTTP_MAX_CLIENTS),
// so neither table can ever hold more entries than that pool allows.
// Same invariant as s_slots_mux: nothing that can block runs while held.
// ---------------------------------------------------------------------------
typedef struct {
    bool                    in_use;
    int                     fd;
    bb_http_handle_t        server;
    bb_data_http_client_t  *client;
} bb_data_http_espidf_ws_slot_t;

static bb_data_http_espidf_ws_slot_t s_ws_slots[BB_DATA_HTTP_ESPIDF_MAX_CLIENTS];
static portMUX_TYPE s_ws_slots_mux = portMUX_INITIALIZER_UNLOCKED;

// Caller must hold s_ws_slots_mux.
static bb_data_http_espidf_ws_slot_t *ws_slot_find_by_client_locked(const bb_data_http_client_t *client)
{
    for (size_t i = 0; i < BB_DATA_HTTP_ESPIDF_MAX_CLIENTS; i++) {
        if (s_ws_slots[i].in_use && s_ws_slots[i].client == client) {
            return &s_ws_slots[i];
        }
    }
    return NULL;
}

// Caller must hold s_ws_slots_mux. Used by the disconnect callback, which
// only carries an fd (bb_ws_server_disconnect_cb_t has no client handle).
static bb_data_http_espidf_ws_slot_t *ws_slot_find_by_fd_locked(int fd)
{
    for (size_t i = 0; i < BB_DATA_HTTP_ESPIDF_MAX_CLIENTS; i++) {
        if (s_ws_slots[i].in_use && s_ws_slots[i].fd == fd) {
            return &s_ws_slots[i];
        }
    }
    return NULL;
}

// Caller must hold s_ws_slots_mux.
static bb_data_http_espidf_ws_slot_t *ws_slot_alloc_locked(void)
{
    for (size_t i = 0; i < BB_DATA_HTTP_ESPIDF_MAX_CLIENTS; i++) {
        if (!s_ws_slots[i].in_use) {
            return &s_ws_slots[i];
        }
    }
    return NULL;
}

// Caller must hold s_ws_slots_mux.
static void ws_slot_free_locked(bb_data_http_espidf_ws_slot_t *slot)
{
    slot->in_use = false;
    slot->fd     = -1;
    slot->server = NULL;
    slot->client = NULL;
}

// ---------------------------------------------------------------------------
// Injected seams
// ---------------------------------------------------------------------------

// Render scratch (fork #1: fixed Kconfig stopgap, graceful degrade). Static
// (not stack) -- the broadcaster is the ONLY task that ever calls this, so
// there is no reentrancy concern, and keeping it off the 4KB task stack
// leaves more headroom for the sweep's own call depth. A render that
// overflows this scratch returns BB_ERR_NO_SPACE from bb_data_render();
// bb_data_http_sweep_step() leaves the key's dirty bit set on any render_fn
// failure, so it is simply retried next sweep (already logged, rate
// limited, by the common core -- see bb_data_http_render_fail_count()).
static uint8_t s_render_scratch[BB_DATA_HTTP_RENDER_SCRATCH_BYTES];

// B1-1424 review item 4: at most ONE client per sweep_step() call may use
// the widened first-frame SO_SNDTIMEO (see
// BB_DATA_HTTP_FIRST_FRAME_TIMEOUT_MS's doc and the per-sweep budget
// arithmetic above) -- this flag is that one-per-sweep allowance. Reset
// true at the top of each broadcaster_task() loop iteration (below,
// BEFORE bb_data_http_sweep_step() is called), consumed (set false) by
// espidf_send_fn's SSE branch -- both only ever run on the single
// broadcaster task, so this needs no lock, same as s_render_scratch above.
static bool s_cold_start_available;

static bb_err_t espidf_render_fn(const char *key, char *buf, size_t cap,
                                  size_t *out_len, void *ctx)
{
    (void)ctx;
    // Broadcaster sweep is query-less -- no per-request params to forward.
    bb_data_render_req_t req = {
        .fmt = BB_FORMAT_JSON, .key = key, .query = NULL,
        .scratch = s_render_scratch, .scratch_cap = sizeof(s_render_scratch),
        .buf = buf, .buf_cap = cap, .out_len = out_len,
    };
    return bb_data_render(&req);
}

static bb_err_t espidf_generation_fn(const char *key, uint32_t *out_gen, void *ctx)
{
    (void)ctx;
    return bb_data_generation(key, out_gen);
}

// `client->is_ws` (read directly off the handle -- bb_data_http_client_t is
// shared with this platform backend via bb_data_http_internal.h) selects
// SSE vs WS framing; it replaces the old is_ws seam parameter (B1-1123
// PR-1), which the transport-neutral seam no longer carries.
//
// `key` (B1-1123 PR-2) is unused here -- both the SSE framing below (`data:
// <payload>\n\n`) and the WS framing (a single raw text frame) carry no
// event name; an `event: <key>` line / topic-qualified WS frame is a
// tracked, out-of-scope wire-format change for a later PR.
static bb_err_t espidf_send_fn(const char *key, const bb_data_http_client_t *client,
                                const void *bytes, size_t len, void *ctx)
{
    (void)ctx;
    (void)key;
    if (!client) {
        return BB_ERR_INVALID_ARG;
    }

    if (client->is_ws) {
        portENTER_CRITICAL(&s_ws_slots_mux);
        bb_data_http_espidf_ws_slot_t *ws_slot = ws_slot_find_by_client_locked(client);
        bb_http_handle_t ws_server = ws_slot ? ws_slot->server : NULL;
        int              ws_fd     = ws_slot ? ws_slot->fd     : -1;
        portEXIT_CRITICAL(&s_ws_slots_mux);
        if (!ws_slot) {
            // B1-1424 HIGH fix (deferred reap): a client whose ws-slot has
            // already been torn down by a concurrent ws_disconnect_cb()
            // (which now only calls bb_data_http_client_request_release(),
            // never bb_data_http_client_release() directly -- see its own
            // doc) is a client already scheduled for release on the
            // broadcaster's OWN next sweep_step() call. RETRIABLE, not
            // fatal: nothing was ever sent (there was no slot to send
            // through), and routing this into espidf_abort_fn would call
            // bb_data_http_client_release() a SECOND time on a client the
            // pending-release reap is about to release anyway -- a
            // duplicate release this deferred-reap design exists
            // specifically to prevent. Leaving the frame queued costs
            // nothing: the pending-release reap at the top of
            // sweep_step()'s per-client loop discards the whole client
            // (queue included) before send_fn is ever called on it again.
            return BB_ERR_TIMEOUT;
        }

        bb_ws_server_frame_t frame = {
            .final   = true,
            .type    = BB_WS_TYPE_TEXT,
            .payload = (uint8_t *)bytes,
            .len     = len,
        };
        // B1-1424/B1-1429: httpd_ws_send_frame_async is fire-and-forget --
        // the real send outcome only surfaces later, on httpd's own async
        // worker task, via the (here-unused) cb parameter. What this call's
        // return value actually reflects is "successfully queued the work
        // item", NOT "the client received the bytes" -- so a failure here
        // means NOTHING was transmitted (the enqueue itself never
        // happened), unlike SSE's synchronous httpd_send_all() below, which
        // can fail with bytes already on the wire. That is exactly
        // bb_data_http_send_fn's RETRIABLE case (bb_data_http.h) -- map an
        // enqueue failure to BB_ERR_TIMEOUT so the core's flush loop
        // retries it on the next sweep instead of tearing the connection
        // down. A frame that enqueues successfully here and then fails to
        // actually reach the peer is retried by neither this contract nor
        // anything else today -- that asymmetry is real and not fully
        // closeable from this call site (see bb_data_http_send_fn's own
        // doc).
        if (bb_ws_server_broadcast_frame_async(ws_server, ws_fd, &frame, NULL, NULL) != BB_OK) {
            return BB_ERR_TIMEOUT;
        }
        return BB_OK;
    }

    portENTER_CRITICAL(&s_slots_mux);
    bb_data_http_espidf_slot_t *slot = slot_find_by_client_locked(client);
    bb_http_request_t *async_req = slot ? slot->async_req : NULL;
    portEXIT_CRITICAL(&s_slots_mux);
    if (!async_req) {
        // Defensive/unreachable in practice (a client with no slot entry
        // was already torn down and would not still be in_use for the
        // core's flush loop to reach) -- treat as FATAL, the safe default
        // for "cannot rule out a partial write" (bb_data_http_send_fn's
        // doc), not BB_ERR_TIMEOUT.
        return BB_ERR_NOT_FOUND;
    }

    // Broadcaster-task-only, so a static frame buffer (not stack) is safe --
    // same reentrancy argument as s_render_scratch above.
    static char s_frame_buf[BB_DATA_HTTP_ESPIDF_FRAME_MAX];
    int n = snprintf(s_frame_buf, sizeof(s_frame_buf), "data: %.*s\n\n",
                     (int)len, (const char *)bytes);
    if (n < 0 || (size_t)n >= sizeof(s_frame_buf)) {
        // A local formatting failure (frame too big for
        // BB_DATA_HTTP_ESPIDF_FRAME_MAX) never touches the socket -- nothing
        // was transmitted, so this is safely RETRIABLE. It will keep
        // failing identically until CONFIG_BB_DATA_HTTP_SEND_FAIL_MAX
        // retriable failures drop this one frame (see
        // bb_data_http_sweep_step()'s flush doc) -- a misconfiguration
        // (BB_DATA_HTTP_ESPIDF_FRAME_MAX too small for a real render), not
        // a transport/stream-integrity issue.
        return BB_ERR_TIMEOUT;
    }
    // B1-1424 review item 4: this client's first-ever send attempt gets a
    // widened SO_SNDTIMEO (BB_DATA_HTTP_FIRST_FRAME_TIMEOUT_MS), IF this
    // sweep_step() call's one-per-sweep cold-start allowance
    // (s_cold_start_available) hasn't already been spent on some other
    // client -- see both symbols' own docs above for the full rationale
    // and per-sweep budget arithmetic. `slot->warmed` is set true right
    // after this attempt regardless of outcome or whether it was widened:
    // the "first attempt" only ever happens once per connection.
    bool cold_start      = !slot->warmed;
    bool use_wide_timeout = cold_start && s_cold_start_available;
    int  send_fd          = bb_http_req_sockfd(async_req);

    if (use_wide_timeout) {
        s_cold_start_available = false;
        struct timeval tv_wide = {
            .tv_sec  = BB_DATA_HTTP_FIRST_FRAME_TIMEOUT_MS / 1000,
            .tv_usec = (BB_DATA_HTTP_FIRST_FRAME_TIMEOUT_MS % 1000) * 1000,
        };
        if (setsockopt(send_fd, SOL_SOCKET, SO_SNDTIMEO, &tv_wide, sizeof(tv_wide)) != 0) {
            bb_log_w(TAG, "fd=%d: SO_SNDTIMEO (first-frame widen) setsockopt failed (errno=%d)", send_fd, errno);
        }
    }

    // B1-1429: bb_http_resp_send_chunk() wraps httpd_resp_send_chunk(),
    // whose httpd_send_all() discards the partial byte count on a failed
    // write -- some header/body bytes can already be on the wire with no
    // way to tell. Any failure here is therefore FATAL, never retried (see
    // bb_data_http_send_fn's return contract) -- a real ESP_ERR_TIMEOUT
    // from the underlying socket layer would otherwise be mistaken for
    // this seam's RETRIABLE sentinel, which is exactly the unsafe case
    // this comment calls out: httpd_send_all() on EAGAIN returns ESP_FAIL,
    // never ESP_ERR_TIMEOUT, so that collision does not occur in practice.
    // This is still a setsockopt/send on the SWEEP task (whichever task
    // called bb_data_http_sweep_step(), i.e. the broadcaster task), never
    // the httpd task -- espidf_send_fn is only ever reached from inside
    // bb_data_http_sweep_step()'s own flush loop.
    bb_err_t send_rc = bb_http_resp_send_chunk(async_req, s_frame_buf, n);

    if (cold_start) {
        slot->warmed = true;
    }

    if (use_wide_timeout) {
        struct timeval tv_steady = {
            .tv_sec  = BB_DATA_HTTP_SEND_TIMEOUT_MS / 1000,
            .tv_usec = (BB_DATA_HTTP_SEND_TIMEOUT_MS % 1000) * 1000,
        };
        if (setsockopt(send_fd, SOL_SOCKET, SO_SNDTIMEO, &tv_steady, sizeof(tv_steady)) != 0) {
            // Unlike the widen-side setsockopt failure above (harmless --
            // worst case this ONE send pays the steady-state timeout
            // instead of the wider one, no different from never having
            // widened at all), a RESTORE failure is treated as FATAL: it
            // would silently leave this one client's socket on the wide
            // FIRST_FRAME_TIMEOUT_MS for every future steady-state send
            // too, turning a one-time cost into a recurring one this
            // client alone keeps paying -- exactly the per-sweep budget
            // violation the compile-time invariant above exists to catch,
            // except this path could reintroduce it silently at runtime.
            // Overrides send_rc even if the send itself just succeeded.
            bb_log_e(TAG, "fd=%d: SO_SNDTIMEO (first-frame restore) failed (errno=%d), forcing fatal", send_fd, errno);
            return BB_ERR_INVALID_STATE;
        }
    }

    return send_rc;
}

// ---------------------------------------------------------------------------
// Broadcaster task (KB 1447: ONE task, bb_task_create BB_TASK_BACKING_DYNAMIC,
// process-lifetime -- the B1-484/492 per-client TCB-reuse hazard bb_sse_writer/
// bb_event_routes must guard against does not exist here).
// ---------------------------------------------------------------------------

// Release + abort a client's bb_data_http slot and async request. Never call
// with s_slots_mux held -- bb_http_req_async_abort() does blocking socket
// teardown.
static void teardown_client(bb_data_http_client_t *client, bb_http_request_t *async_req)
{
    bb_data_http_client_release(client);
    bb_http_req_async_abort(async_req);
}

// Fatal-abort seam (B1-1429, installed via bb_data_http_set_abort_fn() in
// bb_data_http_espidf_start()) -- invoked by the core's flush loop (see
// bb_data_http_sweep_step()'s flush-contract doc, bb_data_http.h) whenever
// espidf_send_fn signals a non-retriable failure. Mirrors
// peer_liveness_prepass()'s own teardown exactly: this backend's own
// slot-table cleanup (SSE: async abort via teardown_client(); WS:
// bb_ws_server_close_client()) THEN bb_data_http_client_release() -- the
// core deliberately never calls client_release() itself before invoking
// this seam, so a backend's side-table entry is always cleared BEFORE the
// client_t slot the core owns can be reused by a new connection (see
// bb_data_http_abort_fn's doc). This bb_data_http_client_release() call is
// exactly the single-task-owning one the deferred-reap design (see
// bb_data_http_client_t's TASK OWNERSHIP doc) requires -- this seam is
// only ever invoked from inside bb_data_http_sweep_step()'s own flush loop
// on the broadcaster task, never from ws_disconnect_cb or any other
// foreign-task context.
//
// The is_ws branch below is defensive/unreachable under espidf_send_fn's
// CURRENT failure mapping (B1-1424 review fix): every WS failure path --
// enqueue failure AND "no ws-slot found" -- now returns BB_ERR_TIMEOUT
// (retriable), never fatal (see espidf_send_fn's WS branch doc above), so
// this seam is never actually reached for an is_ws client today. Kept for
// API completeness (send_fn's contract allows ANY non-BB_OK/non-
// BB_ERR_TIMEOUT return, and a future WS failure classification could
// legitimately need a fatal path) rather than asserting it can't happen.
static void espidf_abort_fn(bb_data_http_client_t *client, void *ctx)
{
    (void)ctx;
    if (!client) return;  // LCOV_EXCL_BR_LINE -- the core's flush loop only ever calls this with the live client `c` it is currently draining; never NULL.

    if (client->is_ws) {  // LCOV_EXCL_BR_LINE -- unreachable under espidf_send_fn's current WS mapping (always retriable, never fatal) -- see this function's own doc above.
        // LCOV_EXCL_START
        portENTER_CRITICAL(&s_ws_slots_mux);
        bb_data_http_espidf_ws_slot_t *slot = ws_slot_find_by_client_locked(client);
        bb_http_handle_t server = slot ? slot->server : NULL;
        int              fd     = slot ? slot->fd     : -1;
        if (slot) ws_slot_free_locked(slot);
        portEXIT_CRITICAL(&s_ws_slots_mux);

        if (server) {
            bb_ws_server_close_client(server, fd);
        }
        bb_log_i(TAG, "ws client fd=%d aborted after fatal send failure", fd);
        bb_data_http_client_release(client);
        return;
        // LCOV_EXCL_STOP
    }

    portENTER_CRITICAL(&s_slots_mux);
    bb_data_http_espidf_slot_t *slot = slot_find_by_client_locked(client);
    bb_http_request_t *async_req = slot ? slot->async_req : NULL;
    int fd = slot ? slot->fd : -1;
    if (slot) slot_free_locked(slot);
    portEXIT_CRITICAL(&s_slots_mux);

    bb_log_i(TAG, "client fd=%d aborted after fatal send failure", fd);
    teardown_client(client, async_req);
}

// Peer-liveness pre-pass (KB 1447: "1/client/sweep before drain"). A single
// non-blocking probe per active slot; a dead peer is released + RST-aborted
// here, BEFORE bb_data_http_sweep_step() would otherwise attempt to drain
// rendered frames to a socket that is never going to accept them.
static void peer_liveness_prepass(void)
{
    for (size_t i = 0; i < BB_DATA_HTTP_ESPIDF_MAX_CLIENTS; i++) {
        portENTER_CRITICAL(&s_slots_mux);
        bb_data_http_espidf_slot_t *slot = &s_slots[i];
        bool in_use = slot->in_use;
        int fd = slot->fd;
        bb_http_request_t *async_req = slot->async_req;
        bb_data_http_client_t *client = slot->client;
        portEXIT_CRITICAL(&s_slots_mux);
        if (!in_use) {
            continue;
        }
        if (!bb_http_req_peer_alive(async_req)) {
            bb_log_i(TAG, "client fd=%d dead, releasing", fd);
            portENTER_CRITICAL(&s_slots_mux);
            slot_free_locked(slot);
            portEXIT_CRITICAL(&s_slots_mux);
            teardown_client(client, async_req);
        }
    }
}

static void broadcaster_task(void *arg)
{
    (void)arg;
    for (;;) {
        bb_task_delay_ms(BB_DATA_HTTP_SWEEP_INTERVAL_MS);
        peer_liveness_prepass();
        s_cold_start_available = true;
        bb_data_http_sweep_step();
    }
}

static bool s_started = false;

bb_err_t bb_data_http_espidf_start(void)
{
    if (s_started) {
        return BB_OK;
    }

    bb_data_http_set_render_fn(espidf_render_fn, NULL);
    bb_data_http_set_generation_fn(espidf_generation_fn, NULL);
    bb_data_http_set_send_fn(espidf_send_fn, NULL);
    bb_data_http_set_abort_fn(espidf_abort_fn, NULL);

    bb_task_config_t cfg = {
        .entry       = broadcaster_task,
        .name        = "bb_data_http",
        .arg         = NULL,
        .stack_bytes = BB_DATA_HTTP_TASK_STACK_BYTES,
        .priority    = BB_DATA_HTTP_TASK_PRIORITY,
        .core        = BB_TASK_CORE_ANY,
        .backing     = BB_TASK_BACKING_DYNAMIC,
        .wdt_arm     = false,
    };
    void *handle = NULL;
    bb_err_t err = bb_task_create(&cfg, &handle);
    if (err != BB_OK) {
        bb_log_e(TAG, "broadcaster task create failed: %d", (int)err);
        return err;
    }

    s_started = true;
    bb_log_i(TAG, "broadcaster task started (sweep=%dms, stack=%dB, prio=%d)",
             BB_DATA_HTTP_SWEEP_INTERVAL_MS, BB_DATA_HTTP_TASK_STACK_BYTES,
             BB_DATA_HTTP_TASK_PRIORITY);
    return BB_OK;
}

// ---------------------------------------------------------------------------
// Connect handler (SYNC httpd task): hardening + SSE headers (inlined --
// fork #2) + async-handoff + client acquire + side-table registration.
// Returns immediately -- no per-connection task, no blocking loop.
// ---------------------------------------------------------------------------
bb_err_t bb_data_http_espidf_client_connect(bb_http_request_t *req,
                                            const char *topic_filter)
{
    if (!req) {
        return BB_ERR_INVALID_ARG;
    }

    bb_http_request_t *async_req = NULL;
    bb_err_t err = bb_http_req_async_handler_begin(req, &async_req);
    if (err != BB_OK) {
        return err;
    }

    int fd = bb_http_req_sockfd(async_req);

    // Hardening (KB 1447, re-derived from bb_sse_writer under the
    // single-broadcaster model): short send timeout so a stalled client
    // costs the shared broadcaster at most BB_DATA_HTTP_SEND_TIMEOUT_MS per
    // sweep, a receive timeout so a half-open peer eventually surfaces, and
    // TCP_NODELAY since SSE frames are small and must arrive promptly.
    struct timeval tv_snd = {
        .tv_sec  = BB_DATA_HTTP_SEND_TIMEOUT_MS / 1000,
        .tv_usec = (BB_DATA_HTTP_SEND_TIMEOUT_MS % 1000) * 1000,
    };
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv_snd, sizeof(tv_snd)) != 0) {
        bb_log_w(TAG, "fd=%d: SO_SNDTIMEO setsockopt failed (errno=%d)", fd, errno);
    }
    struct timeval tv_rcv = {
        .tv_sec  = BB_DATA_HTTP_RECV_TIMEOUT_MS / 1000,
        .tv_usec = (BB_DATA_HTTP_RECV_TIMEOUT_MS % 1000) * 1000,
    };
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv_rcv, sizeof(tv_rcv)) != 0) {
        bb_log_w(TAG, "fd=%d: SO_RCVTIMEO setsockopt failed (errno=%d)", fd, errno);
    }
    int one = 1;
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) != 0) {
        bb_log_w(TAG, "fd=%d: TCP_NODELAY setsockopt failed (errno=%d)", fd, errno);
    }

    // SSE response headers -- inlined (fork #2): bb_sse_writer's near-
    // identical block is a per-client-task model this component does not
    // use and which the B1-1045 cutover deletes wholesale.
    bb_http_resp_set_type(async_req, "text/event-stream");
    bb_http_resp_set_header(async_req, "Cache-Control", "no-cache");
    bb_http_resp_set_header(async_req, "Connection", "keep-alive");
    bb_http_resp_set_header(async_req, "Access-Control-Allow-Origin", "*");

    // B1-1429 (was: a synchronous ": connected\n\n" preamble write here,
    // made non-fatal on failure). Deliberately REMOVED instead, not just
    // made non-fatal: bb_http_resp_send_chunk() wraps ESP-IDF's
    // httpd_resp_send_chunk(), whose FIRST call on a response issues
    // several separate httpd_send_all() writes (status line, each header,
    // end-of-headers CRLF, chunk-size line, body, trailing CRLF) and only
    // marks the response's internal first_chunk_sent AFTER every one of
    // them succeeds. A cold-socket EAGAIN mid-header-block -- exactly the
    // failure this preamble hit on hardware -- can land with some header
    // bytes already on the wire while first_chunk_sent stays false. A
    // "log and continue" version of this write would then let the
    // broadcaster's first real frame call httpd_resp_send_chunk() again on
    // the same async_req, see first_chunk_sent==false, and RE-SEND the
    // entire status line + header block mid-stream: a connection that looks
    // healthy but delivers duplicated/garbled headers interleaved with body
    // bytes forever -- strictly worse than the abort-and-RST this fix set
    // out to replace, and a race that would not reproduce reliably.
    //
    // Routing the preamble through a raw send() on the fd (bypassing
    // httpd's multi-call framing, so a partial write could only ever damage
    // the preamble's own ~14 bytes) was considered and rejected: the
    // preamble is purely cosmetic -- an SSE keep-alive comment nothing
    // downstream depends on receiving -- and httpd commits the response
    // headers on whichever chunk write happens first, preamble or the
    // broadcaster's own first real frame. Simply never sending it removes
    // the hazard instead of working around it. Headers now commit on the
    // first sweep that has content for this client (worst case one
    // BB_DATA_HTTP_SWEEP_INTERVAL_MS after connect), which is not
    // observably different to an SSE/EventSource client -- EventSource
    // fires "open" once response headers arrive, not on any particular
    // body byte.

    bb_data_http_client_t *client = NULL;
    err = bb_data_http_client_acquire_ex(&client, fd, topic_filter, false);
    if (err != BB_OK) {
        bb_http_req_async_abort(async_req);
        return err;
    }

    portENTER_CRITICAL(&s_slots_mux);
    bb_data_http_espidf_slot_t *slot = slot_alloc_locked();
    if (slot) {
        slot->fd       = fd;
        slot->async_req = async_req;
        slot->client   = client;
        slot->warmed   = false;  // first espidf_send_fn call for this client gets the widened first-frame timeout (if this sweep's one-per-sweep allowance is still available)
        slot->in_use   = true;
    }
    portEXIT_CRITICAL(&s_slots_mux);
    if (!slot) {
        teardown_client(client, async_req);
        return BB_ERR_NO_SPACE;
    }

    bb_log_i(TAG, "client fd=%d connected (topic_filter=%s)", fd,
             (topic_filter && topic_filter[0]) ? topic_filter : "*");
    return BB_OK;
}

// ---------------------------------------------------------------------------
// GET /api/events route registration (B1-1215) -- moved verbatim from
// examples/floor's own composition root, which owned this handler with no
// route-descriptor registration alongside it (the bug this fixes: /api/events
// never appeared in /api/openapi.json on any board). See
// bb_data_http_espidf_routes_init()'s doc in bb_data_http.h.
// ---------------------------------------------------------------------------

static bb_err_t events_get_handler(bb_http_request_t *req)
{
    char topic_buf[BB_DATA_HTTP_TOPIC_MAX] = {0};
    const char *topic_filter = NULL;
    if (bb_http_req_query_key_value(req, "topic", topic_buf, sizeof(topic_buf)) == BB_OK) {
        topic_filter = topic_buf;
    }
    return bb_data_http_espidf_client_connect(req, topic_filter);
}

bb_err_t bb_data_http_espidf_routes_init(bb_http_handle_t server)
{
    if (!server) return BB_ERR_INVALID_ARG;

    bb_err_t err = bb_http_register_route(server, BB_HTTP_GET, "/api/events",
                                          events_get_handler);
    if (err != BB_OK) return err;

    return bb_http_register_route_descriptor_only(bb_data_http_events_route());
}

// ---------------------------------------------------------------------------
// WS egress (B1-1050 PR-1): connect/disconnect callbacks + route
// registration. bb_ws_server does the handshake/framing/close work already
// (bb_ws_server_register_endpoint, its connect/disconnect notification
// hooks, httpd_ws_send_frame_async via bb_ws_server_broadcast_frame_async)
// -- this is only the glue that acquires/releases a bb_data_http client slot
// per WS session and records the fd -> (server, client) mapping espidf_
// send_fn's WS branch (above) looks up.
// ---------------------------------------------------------------------------

// Topic filter (B1-1423, tracked follow-up, not this PR's scope):
// bb_ws_server_connect_cb_t is (server, fd, ctx) with no request handle, so
// there is no hook here to read a `?topic=` query param the way
// events_get_handler above does for SSE. Every WS client acquired via this
// callback subscribes to all attached topics (topic_filter=NULL) until
// B1-1423 extends bb_ws_server's connect callback (or an equivalent) with
// enough context to parse one.
static void ws_connect_cb(bb_http_handle_t server, int fd, void *ctx)
{
    (void)ctx;

    bb_data_http_client_t *client = NULL;
    bb_err_t err = bb_data_http_client_acquire_ex(&client, fd, NULL, true);
    if (err != BB_OK) {
        // The WS handshake has already completed by the time this callback
        // fires (see bb_ws_server_set_connect_cb's doc) -- there is no way
        // to refuse the connection from here. The client stays connected at
        // the WS-protocol level but never receives bb_data_http egress
        // (no slot is recorded below), which is the best available fallback
        // for a full client pool / acquire failure.
        bb_log_w(TAG, "ws client fd=%d acquire failed (%d), no egress for it", fd, (int)err);
        return;
    }

    portENTER_CRITICAL(&s_ws_slots_mux);
    bb_data_http_espidf_ws_slot_t *slot = ws_slot_alloc_locked();
    if (slot) {
        slot->fd     = fd;
        slot->server = server;
        slot->client = client;
        slot->in_use = true;
    }
    portEXIT_CRITICAL(&s_ws_slots_mux);
    if (!slot) {
        // Side table full even though the shared client pool had room --
        // release the just-acquired slot rather than leak it silently.
        bb_data_http_client_release(client);
        bb_log_w(TAG, "ws client fd=%d: side table full, no egress for it", fd);
        return;
    }

    bb_log_i(TAG, "ws client fd=%d connected (topic_filter=*)", fd);
}

static void ws_disconnect_cb(int fd, void *ctx)
{
    (void)ctx;

    portENTER_CRITICAL(&s_ws_slots_mux);
    bb_data_http_espidf_ws_slot_t *slot = ws_slot_find_by_fd_locked(fd);
    bb_data_http_client_t *client = slot ? slot->client : NULL;
    if (slot) {
        ws_slot_free_locked(slot);
    }
    portEXIT_CRITICAL(&s_ws_slots_mux);

    // B1-1424 HIGH fix: this callback fires on bb_ws_server's own httpd/ws
    // worker task, NEVER the broadcaster task that owns
    // bb_data_http_sweep_step() -- bb_data_http_client_release() is not
    // cross-task-safe (see its doc, bb_data_http.h): it mutates fields
    // (destroying `outbound`, clearing `in_use`) the broadcaster may be
    // reading/writing for this SAME client, concurrently, on another core,
    // right now, inside its own in-flight sweep_step() call. Deferred
    // reap: only request the release here (a single atomic-flag flip,
    // safe from any task); the broadcaster reaps it -- single-task, no
    // race -- at the top of its own per-client loop on its next
    // sweep_step() call. NULL-safe (see
    // bb_data_http_client_request_release's doc) -- a disconnect for an fd
    // that was never successfully acquired (ws_connect_cb's acquire-
    // failure branch above) is a legitimate no-op here, not a bug.
    bb_data_http_client_request_release(client);
}

// Inbound WS DATA frame handler for the /ws/events egress endpoint.
// bb_data_http has no recv concept -- its three injected seams are render/
// generation/send only (see bb_data_http_internal.h's file header) -- so
// every inbound frame is explicitly discarded here. This is deliberate, not
// a stub left empty by accident: a WS ingress path is a separate, tracked
// design (epic B1-828's ingress axis), and silently doing nothing here
// would risk this handler becoming a de facto (unreviewed) ingress path by
// accident later.
static bb_err_t ws_events_discard_handler(bb_http_request_t *req,
                                          const bb_ws_server_frame_t *frame)
{
    (void)req;
    (void)frame;
    return BB_OK;
}

bb_err_t bb_data_http_espidf_ws_routes_init(bb_http_handle_t server)
{
    if (!server) return BB_ERR_INVALID_ARG;

    // bb_ws_server_set_connect_cb/set_disconnect_cb are process-global --
    // one registration each, a later caller's registration replaces this
    // one (see bb_ws_server.h's doc on both setters). This component owns
    // exactly one WS egress endpoint, so that is fine for bb_data_http
    // itself, but a composition root that also wires its OWN WS endpoint
    // with its own connect/disconnect callbacks (e.g. examples/smoke's /ws
    // echo demo) must be aware whichever of the two calls this or its own
    // setter LAST wins process-wide -- bb_ws_server has no per-endpoint
    // callback registration to avoid this.
    bb_ws_server_set_connect_cb(ws_connect_cb, NULL);
    bb_ws_server_set_disconnect_cb(ws_disconnect_cb, NULL);

    return bb_ws_server_register_endpoint(server, "/ws/events", ws_events_discard_handler);
}
