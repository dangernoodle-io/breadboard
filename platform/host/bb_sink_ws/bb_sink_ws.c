// bb_sink_ws — bb_pub sink that forwards telemetry payloads to WebSocket clients.
// Host implementation: compiled for native tests; no FreeRTOS.
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
// Right-sized to BB_SINK_WS_MAX_CLIENTS (mirrors the ESP-IDF cap, which
// bridges from CONFIG_BB_HTTP_MAX_OPEN_SOCKETS). Slots are keyed by fd via a
// small linear-scan map, not by direct fd indexing — see the ESP-IDF impl's
// header comment for why fd values cannot be used as array indices directly.
//
// Locking: host tests are single-threaded (no FreeRTOS), so this file has no
// real concurrency to guard against — unlike the ESP-IDF impl (which takes a
// FreeRTOS mutex around all s_clients[] access), this file needs no lock to
// stay build-compatible and behaviorally equivalent for tests.

#include "bb_sink_ws.h"
#include "bb_websocket.h"
#include "bb_log.h"
#include "bb_event.h"
#include "bb_json.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

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
static bool s_suspended = false;

// ---------------------------------------------------------------------------
// fd -> slot map
// ---------------------------------------------------------------------------

static int client_slot_find(int fd)
{
    if (fd < 0) return -1;
    for (int i = 0; i < BB_SINK_WS_MAX_CLIENTS; i++) {
        if (s_clients[i].active && s_clients[i].fd == fd) return i;
    }
    return -1;
}

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

// Clear all subscription state for a client (by fd).
static void client_sub_clear(int fd)
{
    int slot = client_slot_find(fd);
    if (slot < 0) return;
    memset(&s_clients[slot], 0, sizeof(s_clients[slot]));
    s_clients[slot].fd = -1;
}

// Parse a JSON string array starting at `arr_start` (first char after the
// opening '[') into a client's subscription slot, replacing its prior set.
// Returns true if parsed successfully (even if array is empty).
static bool parse_topic_array(int fd, const char *arr_start, const char *end)
{
    int slot = client_slot_acquire(fd);
    if (slot < 0) return false;

    char new_subs[BB_SINK_WS_MAX_SUBS][BB_SINK_WS_SUB_MAX_LEN];
    int  new_count = 0;
    const char *r = arr_start;
    while (r < end && *r != ']' && new_count < BB_SINK_WS_MAX_SUBS) {
        while (r < end && *r != '"' && *r != ']') r++;
        if (r >= end || *r == ']') break;
        r++; // skip opening '"'
        const char *str_start = r;
        while (r < end && *r != '"') r++;
        if (r >= end) return false; // unterminated string
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
    const char *end = payload ? payload + len : payload;
    char type_buf[BB_SINK_WS_TYPE_MAX_LEN];

    if (payload && extract_type(payload, len, type_buf, sizeof(type_buf))) {
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
    if (!payload) return false;
    const char *arr = find_array_start(payload, end, "\"sub\":");
    if (!arr) return false;
    return parse_topic_array(fd, arr, end);
}

// Check if a client (fd) is subscribed to a given topic.
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

// ---------------------------------------------------------------------------
// Filtered broadcast
// ---------------------------------------------------------------------------

// Broadcast `buf` (len bytes, topic = channel name) only to clients
// subscribed to that topic.
static bb_err_t broadcast_filtered(const char *topic,
                                   const char *buf, size_t len)
{
    if (s_suspended) return BB_OK;

    bb_websocket_frame_t frame = {
        .final   = true,
        .type    = BB_WS_TYPE_TEXT,
        .payload = (uint8_t *)buf,
        .len     = len,
    };

    bb_err_t last_err = BB_OK;
    for (int fd = 0; fd < BB_WEBSOCKET_MAX_FD; fd++) {
        if (!bb_websocket_is_client(s_ctx.server, fd)) continue;
        if (!client_subscribed(fd, topic)) continue;
        bb_err_t err = bb_websocket_broadcast_frame_async(
            s_ctx.server, fd, &frame, NULL, NULL);
        if (err != BB_OK) last_err = err;
    }
    return last_err;
}

// ---------------------------------------------------------------------------
// Host test helpers (forward decl for s_malloc_fn used in publish path)
// ---------------------------------------------------------------------------

#ifdef BB_SINK_WS_TESTING
// Injectable malloc — defined in the test helpers section below; declared here
// so sink_ws_publish can use it.
static void *(*s_malloc_fn)(size_t) = NULL; // initialised to malloc at first use
static inline void *_sink_malloc(size_t sz) {
    return s_malloc_fn ? s_malloc_fn(sz) : malloc(sz);
}
#else
static inline void *_sink_malloc(size_t sz) { return malloc(sz); }
#endif

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
    char *buf = (char *)_sink_malloc(envelope_len);
    if (!buf) {
        bb_log_w(TAG, "publish: malloc failed for envelope");
        return BB_ERR_NO_SPACE;
    }

    int written = snprintf(buf, envelope_len,
                           "{\"type\":\"push\",\"topic\":\"%s\",\"ts_ms\":%.*s,\"data\":%.*s}",
                           subtopic, (int)ts_len, ts_start, (int)data_len, data_start);
    if (written < 0 || (size_t)written >= envelope_len) {
        bb_log_w(TAG, "publish: snprintf truncated (written=%d cap=%zu)", written, envelope_len);
        free(buf);
        return BB_ERR_NO_SPACE;
    }

    bb_err_t err = broadcast_filtered(subtopic, buf, (size_t)written);
    free(buf);

    if (err != BB_OK) {
        bb_log_d(TAG, "broadcast_filtered '%s' returned %d", subtopic, (int)err);
    }
    return err;
}

// ---------------------------------------------------------------------------
// Suspend / resume
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// Log bb_event subscriber
// ---------------------------------------------------------------------------

static void log_event_cb(bb_event_topic_t topic, int32_t id,
                         const void *data, size_t size, void *user)
{
    (void)topic; (void)id; (void)user;
    if (!data || size == 0) return;
    size_t json_len = size - 1; // strip NUL posted by bb_log_event
    // {"type":"push","topic":"log","data":<json>}: fixed(24) + "log"(3) +
    // "","data":(9) + }(1) + NUL(1) = 38
    size_t envelope_len = json_len + 38;
    char *buf = (char *)_sink_malloc(envelope_len);
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
    free(buf);
}

// ---------------------------------------------------------------------------
// Disconnect notification (mirrors the ESP-IDF impl's fix for stale
// subscription state surviving an fd reuse — see that file's header comment).
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

    bb_err_t reg_err = bb_websocket_register_described_endpoint(server, "/ws", ws_handler, &s_ws_route);
    if (reg_err != BB_OK) return reg_err;

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

// ---------------------------------------------------------------------------
// Host test helpers
// ---------------------------------------------------------------------------

#ifdef BB_SINK_WS_TESTING

void bb_sink_ws_set_malloc(void *(*fn)(size_t))
{
    s_malloc_fn = fn; // NULL → falls back to libc malloc via _sink_malloc
}

void bb_sink_ws_reset_for_test(void)
{
    memset(&s_ctx, 0, sizeof(s_ctx));
    memset(s_clients, 0, sizeof(s_clients));
    for (int i = 0; i < BB_SINK_WS_MAX_CLIENTS; i++) s_clients[i].fd = -1;
    s_malloc_fn = NULL;
    s_suspended = false;
}

// Inject a structured log event as if the log bb_event had fired — broadcasts
// {"type":"push","topic":"log","data":<json>} to all clients subscribed to "log".
void bb_sink_ws_host_inject_log_event(const char *json)
{
    if (!json) return;
    size_t json_len = strlen(json);
    // {"type":"push","topic":"log","data":<json>}: fixed(24) + "log"(3) +
    // "","data":(9) + }(1) + NUL(1) = 38
    size_t envelope_len = json_len + 38;
    char *buf = (char *)_sink_malloc(envelope_len);
    if (!buf) {
        bb_log_w(TAG, "inject_log_event: malloc failed");
        return;
    }
    int written = snprintf(buf, envelope_len,
                           "{\"type\":\"push\",\"topic\":\"log\",\"data\":%.*s}",
                           (int)json_len, json);
    if (written > 0 && (size_t)written < envelope_len) {
        broadcast_filtered("log", buf, (size_t)written);
    }
    free(buf);
}

#endif // BB_SINK_WS_TESTING
