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
#include "bb_data.h"
#include "bb_http_server.h"
#include "bb_task.h"
#include "bb_log.h"

#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/tcp.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

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

// Sweep-budget invariant: the broadcaster's peer-liveness pre-pass plus send
// pass must never be able to block, worst case, longer than one sweep
// interval, or sweeps back up behind a stalled peer indefinitely. Kconfig's
// declared ranges alone don't enforce MAX_CLIENTS*SEND_TIMEOUT_MS staying
// under SWEEP_INTERVAL_MS, so catch a bad combination at compile time
// (default config: 2*20=40ms < 50ms sweep, passes).
#if (BB_DATA_HTTP_ESPIDF_MAX_CLIENTS * BB_DATA_HTTP_SEND_TIMEOUT_MS) >= BB_DATA_HTTP_SWEEP_INTERVAL_MS
#error "bb_data_http: MAX_CLIENTS*SEND_TIMEOUT_MS must stay under SWEEP_INTERVAL_MS -- the broadcaster's per-sweep worst-case block time cannot reach or exceed the sweep cadence"
#endif

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
typedef struct {
    bool                    in_use;
    int                     fd;
    bb_http_request_t      *async_req;
    bb_data_http_client_t  *client;
} bb_data_http_espidf_slot_t;

static bb_data_http_espidf_slot_t s_slots[BB_DATA_HTTP_ESPIDF_MAX_CLIENTS];
static portMUX_TYPE s_slots_mux = portMUX_INITIALIZER_UNLOCKED;

// Caller must hold s_slots_mux.
static bb_data_http_espidf_slot_t *slot_find_by_fd_locked(int fd)
{
    for (size_t i = 0; i < BB_DATA_HTTP_ESPIDF_MAX_CLIENTS; i++) {
        if (s_slots[i].in_use && s_slots[i].fd == fd) {
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

// SSE-only (fork #4: WS deferred to the cutover). A WS client should never
// reach this seam -- bb_data_http_espidf_client_connect() only ever
// acquires with is_ws=false -- but a defensive reject beats silently
// mis-framing WS bytes as an SSE comment.
static bb_err_t espidf_send_fn(int fd, bool is_ws, const void *bytes,
                                size_t len, void *ctx)
{
    (void)ctx;
    if (is_ws) {
        bb_log_w(TAG, "send_fn: WS framing unsupported (SSE-only de-risk)");
        return BB_ERR_UNSUPPORTED;
    }

    portENTER_CRITICAL(&s_slots_mux);
    bb_data_http_espidf_slot_t *slot = slot_find_by_fd_locked(fd);
    bb_http_request_t *async_req = slot ? slot->async_req : NULL;
    portEXIT_CRITICAL(&s_slots_mux);
    if (!async_req) {
        return BB_ERR_NOT_FOUND;
    }

    // Broadcaster-task-only, so a static frame buffer (not stack) is safe --
    // same reentrancy argument as s_render_scratch above.
    static char s_frame_buf[BB_DATA_HTTP_ESPIDF_FRAME_MAX];
    int n = snprintf(s_frame_buf, sizeof(s_frame_buf), "data: %.*s\n\n",
                     (int)len, (const char *)bytes);
    if (n < 0 || (size_t)n >= sizeof(s_frame_buf)) {
        return BB_ERR_NO_SPACE;
    }
    return bb_http_resp_send_chunk(async_req, s_frame_buf, n);
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
        vTaskDelay(pdMS_TO_TICKS(BB_DATA_HTTP_SWEEP_INTERVAL_MS));
        peer_liveness_prepass();
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
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv_snd, sizeof(tv_snd));
    struct timeval tv_rcv = {
        .tv_sec  = BB_DATA_HTTP_RECV_TIMEOUT_MS / 1000,
        .tv_usec = (BB_DATA_HTTP_RECV_TIMEOUT_MS % 1000) * 1000,
    };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv_rcv, sizeof(tv_rcv));
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    // SSE response headers -- inlined (fork #2): bb_sse_writer's near-
    // identical block is a per-client-task model this component does not
    // use and which the B1-1045 cutover deletes wholesale.
    bb_http_resp_set_type(async_req, "text/event-stream");
    bb_http_resp_set_header(async_req, "Cache-Control", "no-cache");
    bb_http_resp_set_header(async_req, "Connection", "keep-alive");
    bb_http_resp_set_header(async_req, "Access-Control-Allow-Origin", "*");

    err = bb_http_resp_send_chunk(async_req, ": connected\n\n", -1);
    if (err != BB_OK) {
        bb_http_req_async_abort(async_req);
        return err;
    }

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
