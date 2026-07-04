// bb_sink_ws — ESP-IDF implementation.
// Forwards serialized telemetry payloads to subscribed WebSocket clients and
// fans the structured "log" bb_event topic to clients subscribed to "log".
// Compiled only on ESP_PLATFORM; the host build uses
// platform/host/bb_sink_ws/bb_sink_ws.c.
//
// Bidirectional envelope
// -----------------------
// Outbound (server -> client):
//   telemetry publish path: {"type":"push","topic":"<subtopic>","ts_ms":<n>,"data":{...}}
//     (B1-570 PR-3: every payload delivered to a bb_pub sink is now enveloped
//     as {"ts_ms":n,"data":{...}} — sink_ws_publish HOISTS ts_ms/data to the
//     frame level instead of nesting the whole envelope under "data", so the
//     dashboard reads a uniform {ts_ms,data} shape across REST/SSE/WS.)
//   log path (raw bb_event, not enveloped by bb_cache):
//     {"type":"push","topic":"log","data":{...}}
//
// Inbound (client -> server), demuxed by "type" from the start:
//   {"type":"sub","topic":["telemetry","events","log",...]}
//     -> replaces the client's entire subscription set.
//   any other "type" (e.g. "cmd")
//     -> RESERVED, ignored-with-log. No command execution path exists yet;
//        this keeps bb_sink_ws egress-pure and reserves the inbound command
//        channel for a future component.
//   no "type" key at all
//     -> legacy back-compat: {"sub":["telemetry",...]} is still accepted.
//
// Matching rules (checked in order):
//   1. Exact match:  client subscribed to "power"  → receives topic="power"
//   2. Coarse group: client subscribed to "telemetry" → receives any topic
//      that maps to a bb_pub telemetry subtopic (mining, pool, info, wifi,
//      fan, power, thermal, …).  All bb_pub subtopics that are NOT "events"
//      or "log" are considered part of the "telemetry" group.
//   3. "events" group: receives topic values that come from the events bus
//      (subtopic == "events").
//   4. "log" channel: exact opt-in; clients subscribe with
//      {"type":"sub","topic":["log"]}.
//      Receives {"type":"push","topic":"log","data":{...}} from bb_log_event.
//   5. If the client has NO subscriptions yet (e.g., just connected and has
//      not sent a sub frame), it receives NOTHING (subscription required).
//
// Per-client subscription state
// -----------------------------
// Right-sized to BB_SINK_WS_MAX_CLIENTS (bridged from
// CONFIG_BB_HTTP_MAX_OPEN_SOCKETS — no more than that many fds can ever be
// live WS clients, since WS shares the httpd socket pool with HTTP/SSE).
// Slots are keyed by fd via a small linear-scan map, NOT by direct fd
// indexing: LWIP socket fds are offset (LWIP_SOCKET_OFFSET =
// FD_SETSIZE - CONFIG_LWIP_MAX_SOCKETS), so live fd values commonly land in
// the FD_SETSIZE-CONFIG_LWIP_MAX_SOCKETS..FD_SETSIZE-1 range — far above
// BB_SINK_WS_MAX_CLIENTS. Indexing s_clients[fd] directly would need an
// array sized to BB_WEBSOCKET_MAX_FD (FD_SETSIZE, 64) regardless of how few
// concurrent clients are allowed; the fd->slot map avoids that while still
// bounding memory to the real concurrency cap.
//
// Log channel
// -----------
// bb_sink_ws_init subscribes to the "log" bb_event topic (registered by
// bb_log_event at order 4).  log_event_cb builds the
//   {"type":"push","topic":"log","data":{...}}
// envelope and fans it to clients subscribed to "log".  s_suspended gate
// is checked in log_event_cb to match the telemetry publish behaviour.
//
// Locking
// -------
// s_clients[] (the fd->slot map and per-client subscription state) is
// read/written from multiple tasks: the httpd worker task(s) delivering
// inbound WS frames and disconnect notifications, and the bb_pub worker task
// driving the periodic broadcast tick. s_clients_lock (a FreeRTOS mutex,
// same idiom as bb_net_health's s_cache_lock) guards every access. Critical
// sections are kept tight: broadcast_filtered takes the lock only to build a
// snapshot of matching fds, then releases it before calling
// bb_websocket_broadcast_frame_async — the actual httpd send never runs
// under the lock.
#ifdef ESP_PLATFORM

#include "bb_sink_ws.h"
#include "bb_websocket.h"
#include "bb_log.h"
#include "bb_event.h"
#include "bb_json.h"
#include "bb_mem.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "sdkconfig.h"

#ifdef CONFIG_BB_HTTP_MAX_OPEN_SOCKETS
#define BB_SINK_WS_MAX_CLIENTS CONFIG_BB_HTTP_MAX_OPEN_SOCKETS
#endif
#ifndef BB_SINK_WS_MAX_CLIENTS
#define BB_SINK_WS_MAX_CLIENTS 5
#endif

static const char *TAG = "bb_sink_ws";

// ---------------------------------------------------------------------------
// Subscription table
// ---------------------------------------------------------------------------

// Max subscription strings per client.
#define BB_SINK_WS_MAX_SUBS 8
// Max length of a single subscription string (channel name).
#define BB_SINK_WS_SUB_MAX_LEN 32
// Max length of the inbound envelope "type" value.
#define BB_SINK_WS_TYPE_MAX_LEN 16

typedef struct {
    bool active;                                 // slot is in use
    int  fd;                                      // owning fd; -1 when free
    char subs[BB_SINK_WS_MAX_SUBS][BB_SINK_WS_SUB_MAX_LEN];
    int  sub_count;
} client_sub_t;

static client_sub_t s_clients[BB_SINK_WS_MAX_CLIENTS];

// Guards all access to s_clients[]. Created in bb_sink_ws_init.
static SemaphoreHandle_t s_clients_lock;

// ---------------------------------------------------------------------------
// fd -> slot map
//
// client_slot_find / client_slot_acquire are internal helpers called ONLY
// from within a section already holding s_clients_lock (parse_topic_array,
// client_sub_clear, client_subscribed via broadcast_filtered) — they do NOT
// take the lock themselves, since s_clients_lock is a plain (non-recursive)
// FreeRTOS mutex.
// ---------------------------------------------------------------------------

// Find the slot currently owned by fd, or -1 if none.
static int client_slot_find(int fd)
{
    if (fd < 0) return -1;
    for (int i = 0; i < BB_SINK_WS_MAX_CLIENTS; i++) {
        if (s_clients[i].active && s_clients[i].fd == fd) return i;
    }
    return -1;
}

// Find (or allocate) the slot for fd. Returns -1 if fd is invalid or the
// pool is exhausted (should not happen in practice: BB_SINK_WS_MAX_CLIENTS
// tracks CONFIG_BB_HTTP_MAX_OPEN_SOCKETS, the hard cap on concurrent
// sockets across the whole httpd server).
static int client_slot_acquire(int fd)
{
    if (fd < 0) return -1;
    int existing = client_slot_find(fd);
    if (existing >= 0) return existing;
    for (int i = 0; i < BB_SINK_WS_MAX_CLIENTS; i++) {
        if (!s_clients[i].active) {
            memset(&s_clients[i], 0, sizeof(s_clients[i]));
            s_clients[i].active = true;
            s_clients[i].fd     = fd;
            return i;
        }
    }
    bb_log_w(TAG, "client slot pool exhausted (cap=%d), dropping sub for fd=%d",
             BB_SINK_WS_MAX_CLIENTS, fd);
    return -1;
}

// Clear all subscription state for a client (by fd). Takes s_clients_lock.
static void client_sub_clear(int fd)
{
    xSemaphoreTake(s_clients_lock, portMAX_DELAY);
    int slot = client_slot_find(fd);
    if (slot >= 0) {
        memset(&s_clients[slot], 0, sizeof(s_clients[slot]));
        s_clients[slot].fd = -1;
    }
    xSemaphoreGive(s_clients_lock);
}

// Parse a JSON string array starting at `arr_start` (first char after the
// opening '[') into a client's subscription slot, replacing its prior set.
// Returns true if parsed successfully (even if array is empty).
// Takes s_clients_lock for the full parse+commit (bounded, non-blocking work
// only — no I/O under the lock).
static bool parse_topic_array(int fd, const char *arr_start, const char *end)
{
    xSemaphoreTake(s_clients_lock, portMAX_DELAY);

    int slot = client_slot_acquire(fd);
    if (slot < 0) {
        xSemaphoreGive(s_clients_lock);
        return false;
    }

    char new_subs[BB_SINK_WS_MAX_SUBS][BB_SINK_WS_SUB_MAX_LEN];
    int  new_count = 0;
    const char *r = arr_start;
    while (r < end && *r != ']' && new_count < BB_SINK_WS_MAX_SUBS) {
        while (r < end && *r != '"' && *r != ']') r++;
        if (r >= end || *r == ']') break;
        r++; // skip opening '"'
        const char *str_start = r;
        while (r < end && *r != '"') r++;
        if (r >= end) {
            xSemaphoreGive(s_clients_lock);
            return false; // unterminated string
        }
        size_t str_len = (size_t)(r - str_start);
        if (str_len >= BB_SINK_WS_SUB_MAX_LEN) str_len = BB_SINK_WS_SUB_MAX_LEN - 1;
        memcpy(new_subs[new_count], str_start, str_len);
        new_subs[new_count][str_len] = '\0';
        new_count++;
        r++; // skip closing '"'
    }

    client_sub_t *cs = &s_clients[slot];
    cs->sub_count = new_count;
    for (int i = 0; i < new_count; i++) {
        memcpy(cs->subs[i], new_subs[i], BB_SINK_WS_SUB_MAX_LEN);
    }
    xSemaphoreGive(s_clients_lock);
    return true;
}

// Locate `"key":` in payload and return a pointer to the value that follows,
// with any whitespace between the colon and the value already skipped.
// Tolerates pretty-printed JSON (e.g. Python's json.dumps default ": "
// separator) — NULL if the key is absent.
static const char *find_key_value(const char *payload, const char *end, const char *key)
{
    size_t key_len = strlen(key);
    for (const char *q = payload; q + key_len <= end; q++) {
        if (memcmp(q, key, key_len) == 0) {
            const char *v = q + key_len;
            while (v < end && (*v == ' ' || *v == '\t' || *v == '\n' || *v == '\r')) v++;
            return v;
        }
    }
    return NULL;
}

// Locate the byte range right after `"key":[` in payload (whitespace-tolerant
// around the colon), or NULL if absent or not an array.
static const char *find_array_start(const char *payload, const char *end, const char *key)
{
    const char *v = find_key_value(payload, end, key);
    if (!v || v >= end || *v != '[') return NULL;
    return v + 1;
}

// Extract the envelope "type" value into out (NUL-terminated). Returns false
// if no "type" key is present in payload.
static bool extract_type(const char *payload, size_t len, char *out, size_t out_cap)
{
    const char *end = payload + len;
    const char *v = find_key_value(payload, end, "\"type\":");
    if (!v || v >= end || *v != '"') return false;
    v++; // skip opening quote
    const char *v_end = v;
    while (v_end < end && *v_end != '"') v_end++;
    if (v_end >= end) return false;
    size_t vlen = (size_t)(v_end - v);
    if (vlen >= out_cap) vlen = out_cap - 1;
    memcpy(out, v, vlen);
    out[vlen] = '\0';
    return true;
}

// Demux an inbound TEXT frame by envelope "type" and dispatch:
//   "sub"      -> parse "topic":[...] into the client's subscription slot.
//   other      -> RESERVED (e.g. "cmd"); ignored-with-log, no execution path.
//   (no type)  -> legacy back-compat {"sub":[...]}.
static bool dispatch_inbound_frame(int fd, const char *payload, size_t len)
{
    const char *end = payload + len;
    char type_buf[BB_SINK_WS_TYPE_MAX_LEN];

    if (extract_type(payload, len, type_buf, sizeof(type_buf))) {
        if (strcmp(type_buf, "sub") == 0) {
            const char *arr = find_array_start(payload, end, "\"topic\":");
            if (!arr) return false;
            return parse_topic_array(fd, arr, end);
        }
        // RESERVED: command/other envelope types have no execution path yet.
        bb_log_d(TAG, "ws: ignoring inbound frame type '%s' (reserved)", type_buf);
        return true;
    }

    // Legacy back-compat: {"sub":[...]} with no envelope "type".
    const char *arr = find_array_start(payload, end, "\"sub\":");
    if (!arr) return false;
    return parse_topic_array(fd, arr, end);
}

// Check if a client (fd) is subscribed to a given topic.
// Internal helper — called ONLY from within a section already holding
// s_clients_lock (broadcast_filtered's scan phase below); does not lock
// itself, since s_clients_lock is a plain (non-recursive) FreeRTOS mutex.
//
// Matching logic:
//   - Exact name match: sub == topic
//   - Coarse group "telemetry": matches any topic that is NOT "events" and NOT "log"
//   - Coarse group "events":    matches topic == "events"
//   - "log" is exact opt-in only; not covered by any coarse group
static bool client_subscribed(int fd, const char *topic)
{
    int slot = client_slot_find(fd);
    if (slot < 0) return false;
    const client_sub_t *cs = &s_clients[slot];
    if (cs->sub_count == 0) return false;

    for (int i = 0; i < cs->sub_count; i++) {
        const char *sub = cs->subs[i];
        // Exact match
        if (strcmp(sub, topic) == 0) return true;
        // Coarse group: "telemetry" covers anything that is not events or log
        if (strcmp(sub, "telemetry") == 0 &&
            strcmp(topic, "events")  != 0 &&
            strcmp(topic, "log")     != 0) return true;
        // Coarse group: "events" covers topic="events"
        if (strcmp(sub, "events") == 0 && strcmp(topic, "events") == 0) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// WebSocket handler and route descriptor
// ---------------------------------------------------------------------------

static bb_err_t ws_handler(bb_http_request_t *req, const bb_websocket_frame_t *frame)
{
    // Only process TEXT frames carrying envelope commands.
    if (!frame || frame->type != BB_WS_TYPE_TEXT || !frame->payload || frame->len == 0) {
        return BB_OK;
    }

    int fd = bb_websocket_req_fd(req);
    // Attempt to demux and dispatch; ignore malformed frames silently.
    dispatch_inbound_frame(fd, (const char *)frame->payload, frame->len);
    return BB_OK;
}

static const bb_route_t s_ws_route = {
    .method  = BB_HTTP_GET,
    .path    = "/ws",
    .summary = "WebSocket telemetry stream",
    .tag     = "telemetry",
};

// ---------------------------------------------------------------------------
// Context
// ---------------------------------------------------------------------------

typedef struct {
    bb_http_handle_t server;
} sink_ws_ctx_t;

static sink_ws_ctx_t s_ctx;
static bool s_suspended = false;

// ---------------------------------------------------------------------------
// Filtered broadcast
// ---------------------------------------------------------------------------

// Broadcast `buf` (len bytes, topic = channel name) only to clients
// subscribed to that topic.
//
// Two-phase to keep the critical section tight: scan+match under
// s_clients_lock into a bounded fd snapshot, release the lock, then fire the
// (potentially slower) async sends outside it — bb_websocket_broadcast_frame_async
// must never run while s_clients_lock is held.
//
// TOCTOU note: the fd snapshot is taken here, but the actual
// httpd_ws_send_frame_async fires later from a deferred httpd_queue_work
// item (bb_websocket.c's async_send_worker). That worker re-validates each
// fd is still a live WS client immediately before sending, closing the
// common closed-fd race. It does NOT close the narrower window where the
// same fd number was reused by a NEW WS client in between — that residual
// case delivers at most one stray broadcast telemetry frame (broadcast-
// public data, bounded by the worker-queue latency) to the wrong client and
// is accepted/tracked as a follow-up, not a generation-counter fix here.
static bb_err_t broadcast_filtered(const char *topic,
                                   const char *buf, size_t len)
{
    bb_websocket_frame_t frame = {
        .final   = true,
        .type    = BB_WS_TYPE_TEXT,
        .payload = (uint8_t *)buf,
        .len     = len,
    };

    int matched_fds[BB_SINK_WS_MAX_CLIENTS];
    int matched_count = 0;

    xSemaphoreTake(s_clients_lock, portMAX_DELAY);
    for (int fd = 0; fd < BB_WEBSOCKET_MAX_FD; fd++) {
        if (!bb_websocket_is_client(s_ctx.server, fd)) continue;
        if (!client_subscribed(fd, topic)) continue;
        if (matched_count < BB_SINK_WS_MAX_CLIENTS) {
            matched_fds[matched_count++] = fd;
        }
    }
    xSemaphoreGive(s_clients_lock);

    bb_err_t last_err = BB_OK;
    for (int i = 0; i < matched_count; i++) {
        bb_err_t err = bb_websocket_broadcast_frame_async(
            s_ctx.server, matched_fds[i], &frame, NULL, NULL);
        if (err != BB_OK) last_err = err;
    }
    return last_err;
}

// ---------------------------------------------------------------------------
// Publish callback
// ---------------------------------------------------------------------------

// Extract subtopic: the part after the 2nd '/' in "<prefix>/<hostname>/<subtopic>".
static const char *extract_subtopic(const char *topic)
{
    const char *p = topic;
    int slashes = 0;
    while (*p && slashes < 2) {
        if (*p == '/') slashes++;
        p++;
    }
    return p;
}

static bb_err_t sink_ws_publish(void *ctx, const char *topic,
                                const char *payload, int len, bool retain)
{
    (void)ctx;
    (void)retain;   /* WebSocket has no retain concept */

    const char *subtopic = extract_subtopic(topic);

    // Split the {"ts_ms":<n>,"data":{...}} envelope (B1-570 PR-3 — every
    // payload bb_pub delivers to a sink is enveloped this way, telem/cache-
    // backed AND legacy sources alike) via the shared bb_json helper, so
    // sink_ws_publish can hoist ts_ms/data to the WS frame level instead of
    // nesting the whole envelope under "data".
    const char *ts_start = NULL, *data_start = NULL;
    size_t ts_len = 0, data_len = 0;
    if (!bb_json_envelope_split(payload, len, &ts_start, &ts_len, &data_start, &data_len)) {
        bb_log_w(TAG, "publish: '%s' payload missing {ts_ms,data} envelope", subtopic);
        return BB_ERR_INVALID_ARG;
    }

    // Hoisted frame: {"type":"push","topic":"<subtopic>","ts_ms":<n>,"data":{...}}
    // overhead: {"type":"push","topic":"(25) + subtopic + "","ts_ms":(10) +
    // ts_digits + ,"data":(8) + data_bytes + }(1) + NUL(1) = 44
    size_t subtopic_len = strlen(subtopic);
    size_t envelope_len = subtopic_len + ts_len + data_len + 44;
    char *buf = (char *)bb_malloc_prefer_spiram(envelope_len);
    if (!buf) {
        bb_log_w(TAG, "publish: malloc failed for envelope");
        return BB_ERR_NO_SPACE;
    }

    int written = snprintf(buf, envelope_len,
                           "{\"type\":\"push\",\"topic\":\"%s\",\"ts_ms\":%.*s,\"data\":%.*s}",
                           subtopic, (int)ts_len, ts_start, (int)data_len, data_start);
    if (written < 0 || (size_t)written >= envelope_len) {
        bb_log_w(TAG, "publish: snprintf truncated (written=%d cap=%zu)", written, envelope_len);
        bb_mem_free(buf);
        return BB_ERR_NO_SPACE;
    }

    bb_err_t err = broadcast_filtered(subtopic, buf, (size_t)written);
    bb_mem_free(buf);

    if (err != BB_OK) {
        bb_log_d(TAG, "broadcast_filtered '%s' returned %d", subtopic, (int)err);
    }
    return err;
}

// ---------------------------------------------------------------------------
// Log bb_event subscriber
// ---------------------------------------------------------------------------

static void log_event_cb(bb_event_topic_t topic, int32_t id,
                         const void *data, size_t size, void *user)
{
    (void)topic; (void)id; (void)user;
    if (s_suspended) return;
    if (!data || size == 0) return;
    size_t json_len = size - 1; // strip NUL posted by bb_log_event
    // {"type":"push","topic":"log","data":<json>}: fixed(24) + "log"(3) +
    // "","data":(9) + }(1) + NUL(1) = 38
    size_t envelope_len = json_len + 38;
    char *buf = (char *)bb_malloc_prefer_spiram(envelope_len);
    if (!buf) {
        bb_log_w(TAG, "log_event_cb: malloc failed");
        return;
    }
    int written = snprintf(buf, envelope_len,
                           "{\"type\":\"push\",\"topic\":\"log\",\"data\":%.*s}",
                           (int)json_len, (const char *)data);
    if (written > 0 && (size_t)written < envelope_len) {
        broadcast_filtered("log", buf, (size_t)written);
    }
    bb_mem_free(buf);
}

// ---------------------------------------------------------------------------
// Disconnect notification (fixes stale subscription state surviving an fd
// reuse — an LWIP fd freed on disconnect can be reissued to a brand new
// connection before bb_sink_ws_suspend() ever runs, otherwise bleeding the
// prior client's subscription filter onto the new one).
// ---------------------------------------------------------------------------

static void ws_disconnect_cb(int fd, void *ctx)
{
    (void)ctx;
    client_sub_clear(fd);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bb_err_t bb_sink_ws_init(bb_http_handle_t server, bb_pub_sink_t *out)
{
    if (!out) return BB_ERR_INVALID_ARG;

    if (!s_clients_lock) {
        s_clients_lock = xSemaphoreCreateMutex();
        if (!s_clients_lock) {
            bb_log_e(TAG, "clients lock create failed");
            return BB_ERR_NO_SPACE;
        }
    }

    bb_err_t reg_err = bb_websocket_register_described_endpoint(server, "/ws", ws_handler, &s_ws_route);
    if (reg_err != BB_OK) {
        bb_log_e(TAG, "register /ws failed: %d", (int)reg_err);
        return reg_err;
    }

    bb_websocket_set_disconnect_cb(ws_disconnect_cb, NULL);

    s_ctx.server = server;

    // Subscribe to the "log" bb_event topic (registered by bb_log_event at order 4).
    bb_event_topic_t lt;
    if (bb_event_topic_lookup("log", &lt) == BB_OK) {
        bb_err_t sub_err = bb_event_subscribe(lt, log_event_cb, NULL, NULL);
        if (sub_err != BB_OK) {
            bb_log_w(TAG, "log event subscribe failed: %d", (int)sub_err);
        }
    } else {
        bb_log_d(TAG, "log topic not yet registered, skipping ws subscription");
    }

    out->publish       = sink_ws_publish;
    out->ctx           = &s_ctx;
    out->transport     = "websocket";
    out->tls           = false;
    out->subscribe     = NULL;
    out->subscribe_ctx = NULL;
    return BB_OK;
}

bb_err_t bb_sink_ws_suspend(void)
{
    if (s_suspended) return BB_OK;
    for (int fd = 0; fd < BB_WEBSOCKET_MAX_FD; fd++) {
        if (bb_websocket_is_client(s_ctx.server, fd)) {
            bb_websocket_close_client(s_ctx.server, fd);
            client_sub_clear(fd);
        }
    }
    s_suspended = true;
    return BB_OK;
}

void bb_sink_ws_resume(void)
{
    s_suspended = false;
}

#ifdef BB_SINK_WS_TESTING
void bb_sink_ws_reset_for_test(void)
{
    memset(&s_ctx, 0, sizeof(s_ctx));
    memset(s_clients, 0, sizeof(s_clients));
    for (int i = 0; i < BB_SINK_WS_MAX_CLIENTS; i++) s_clients[i].fd = -1;
    s_suspended = false;
}
#endif

#endif /* ESP_PLATFORM */
