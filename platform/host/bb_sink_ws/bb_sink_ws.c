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
#include "bb_ws_server.h"
#include "bb_log.h"
#include "bb_event.h"
#include "bb_json.h"
#include "bb_cache.h"
#include "bb_cache_reactive.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h>
#include <stdatomic.h>

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
// Read from three call paths (bb_pub-tick reader, reactive on_change on the
// writer, connect callback) -- host tests are single-threaded, but mirrors
// the espidf copy's _Atomic to document the accepted-staleness contract
// consistently across both platform copies (see the espidf file's comment).
static _Atomic bool s_suspended = false;

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
// (B) payload is never NULL here: dispatch_inbound_frame's only caller
// (ws_handler) already returns early on !frame->payload, so every
// payload-nullness check below is dead-by-construction defensive code —
// LCOV_EXCL_BR_LINE on each.
static bool dispatch_inbound_frame(int fd, const char *payload, size_t len)
{
    const char *end = payload ? payload + len : payload;  // LCOV_EXCL_BR_LINE — payload always non-NULL, see comment above
    char type_buf[BB_SINK_WS_TYPE_MAX_LEN];

    if (payload && extract_type(payload, len, type_buf, sizeof(type_buf))) {  // LCOV_EXCL_BR_LINE — "payload &&" always true, see comment above
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
    if (!payload) return false;  // LCOV_EXCL_BR_LINE — payload always non-NULL, see comment above
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
        // Coarse group: "events" covers topic="events".
        // (B) the second operand can never be reached true here: whenever
        // sub == topic == "events", the exact-match check two lines above
        // already returns true first, so this line only ever runs with
        // topic != "events" — dead-by-construction.
        if (strcmp(sub, "events") == 0 && strcmp(topic, "events") == 0) return true;  // LCOV_EXCL_BR_LINE — see comment above
    }
    return false;
}

// ---------------------------------------------------------------------------
// WebSocket handler and route descriptor
// ---------------------------------------------------------------------------

static bb_err_t ws_handler(bb_http_request_t *req, const bb_ws_server_frame_t *frame)
{
    // Only process TEXT frames carrying envelope commands.
    // (B) !frame is never true: the bb_ws_server transport (and the host
    // mock harness) always invokes a registered handler with a non-NULL
    // frame — dead-by-construction defensive check.
    if (!frame || frame->type != BB_WS_TYPE_TEXT || !frame->payload || frame->len == 0) {  // LCOV_EXCL_BR_LINE — see comment above
        return BB_OK;
    }

    int fd = bb_ws_server_req_fd(req);
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

    bb_ws_server_frame_t frame = {
        .final   = true,
        .type    = BB_WS_TYPE_TEXT,
        .payload = (uint8_t *)buf,
        .len     = len,
    };

    bb_err_t last_err = BB_OK;
    for (int fd = 0; fd < BB_WS_SERVER_MAX_FD; fd++) {
        if (!bb_ws_server_is_client(s_ctx.server, fd)) continue;
        if (!client_subscribed(fd, topic)) continue;
        bb_err_t err = bb_ws_server_broadcast_frame_async(
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
    // (B) envelope_len is computed above to exactly fit this format string
    // (subtopic_len + ts_len + data_len + fixed literal overhead) — snprintf
    // can never truncate or error against a buffer sized to its own inputs;
    // unreachable without corrupting the size arithmetic above.
    if (written < 0 || (size_t)written >= envelope_len) {  // LCOV_EXCL_BR_LINE — see comment above
        // LCOV_EXCL_START — body of the unreachable branch above
        bb_log_w(TAG, "publish: snprintf truncated (written=%d cap=%zu)", written, envelope_len);
        free(buf);
        return BB_ERR_NO_SPACE;
        // LCOV_EXCL_STOP
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
    for (int fd = 0; fd < BB_WS_SERVER_MAX_FD; fd++) {
        if (bb_ws_server_is_client(s_ctx.server, fd)) {
            bb_ws_server_close_client(s_ctx.server, fd);
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

// (B) ESP-only reachability: log_event_cb is only ever invoked by bb_event
// as the subscriber callback for the "log" topic, registered in
// bb_sink_ws_init() via bb_event_topic_lookup("log", ...). That topic is
// registered exclusively by platform/espidf/bb_log/bb_log_event.c, which is
// compiled only under ESP_PLATFORM — on host there is no "log" topic to
// subscribe to, so this callback body never runs. Host tests exercise the
// equivalent broadcast behavior via the host-only test-injection helper
// bb_sink_ws_host_inject_log_event() below, which calls broadcast_filtered
// directly rather than through this ESP-only callback.
// LCOV_EXCL_START
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
// LCOV_EXCL_STOP

// ---------------------------------------------------------------------------
// bb_cache_reactive change-driven deltas + snapshot-on-connect (B1-589
// PR-4b) — mirrors the ESP-IDF impl; see that file's header comment for the
// full rationale.
// ---------------------------------------------------------------------------

static char *format_push_frame(const char *topic, int64_t ts_ms,
                                const char *data, size_t data_len, size_t *out_len)
{
    size_t topic_len = strlen(topic);
    size_t envelope_len = topic_len + 20 + data_len + 65;
    char *buf = (char *)_sink_malloc(envelope_len);
    if (!buf) return NULL;

    int written = snprintf(buf, envelope_len,
                           "{\"type\":\"push\",\"topic\":\"%s\",\"ts_ms\":%" PRId64 ",\"data\":%.*s}",
                           topic, ts_ms, (int)data_len, data);
    // (B) envelope_len is computed above to exactly fit this format string
    // (topic_len + data_len + fixed literal overhead) — snprintf can never
    // truncate or error against a buffer sized to its own inputs; see the
    // identical invariant in sink_ws_publish above.
    if (written < 0 || (size_t)written >= envelope_len) {  // LCOV_EXCL_BR_LINE — see comment above
        // LCOV_EXCL_START — body of the unreachable branch above
        free(buf);
        return NULL;
        // LCOV_EXCL_STOP
    }
    // (B) out_len is never NULL: both call sites (reactive_on_change,
    // snapshot_key_to_fd) pass &out_len from a local stack variable.
    if (out_len) *out_len = (size_t)written;  // LCOV_EXCL_BR_LINE — see comment above
    return buf;
}

static void reactive_on_change(const char *key, const char *json, size_t len,
                               int64_t ts_ms, void *ctx)
{
    (void)ctx;
    if (s_suspended) return;

    size_t out_len = 0;
    char *buf = format_push_frame(key, ts_ms, json, len, &out_len);
    if (!buf) {
        bb_log_w(TAG, "reactive_on_change: format failed for '%s'", key);
        return;
    }
    broadcast_filtered(key, buf, out_len);
    free(buf);
}

#define BB_SINK_WS_SNAPSHOT_BUF_MAX 512

typedef struct {
    bb_http_handle_t server;
    int               fd;
} snapshot_ctx_t;

static void snapshot_key_to_fd(const char *key, void *ctx_v)
{
    snapshot_ctx_t *sc = (snapshot_ctx_t *)ctx_v;

    char envbuf[BB_SINK_WS_SNAPSHOT_BUF_MAX];
    size_t env_len = 0;
    if (bb_cache_get_serialized(key, envbuf, sizeof(envbuf), &env_len) != BB_OK) {
        bb_log_d(TAG, "snapshot: get_serialized('%s') unavailable, skipping", key);
        return;
    }

    const char *ts_start = NULL, *data_start = NULL;
    size_t ts_len = 0, data_len = 0;
    // (B) bb_cache_get_serialized() always emits a well-formed
    // {"ts_ms":N,"data":<json>} envelope by construction (snprintf against a
    // fixed literal format string wrapping an already-validated JSON "data"
    // blob) — bb_json_envelope_split can never fail against its own output.
    if (!bb_json_envelope_split(envbuf, (int)env_len, &ts_start, &ts_len, &data_start, &data_len)) {  // LCOV_EXCL_BR_LINE — see comment above
        // LCOV_EXCL_START — body of the unreachable branch above
        bb_log_w(TAG, "snapshot: envelope split failed for '%s'", key);
        return;
        // LCOV_EXCL_STOP
    }

    char ts_buf[24];
    memcpy(ts_buf, ts_start, ts_len);
    ts_buf[ts_len] = '\0';
    int64_t ts_ms = (int64_t)strtoll(ts_buf, NULL, 10);

    size_t out_len = 0;
    char *buf = format_push_frame(key, ts_ms, data_start, data_len, &out_len);
    if (!buf) {
        bb_log_w(TAG, "snapshot: format failed for '%s'", key);
        return;
    }

    bb_ws_server_frame_t frame = {
        .final   = true,
        .type    = BB_WS_TYPE_TEXT,
        .payload = (uint8_t *)buf,
        .len     = out_len,
    };
    bb_ws_server_broadcast_frame_async(sc->server, sc->fd, &frame, NULL, NULL);
    free(buf);
}

static void ws_connect_cb(bb_http_handle_t server, int fd, void *ctx)
{
    (void)ctx;
    if (s_suspended) return;

    snapshot_ctx_t sc = { .server = server, .fd = fd };
    bb_cache_foreach(snapshot_key_to_fd, &sc);
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

    bb_err_t reg_err = bb_ws_server_register_described_endpoint(server, "/ws", ws_handler, &s_ws_route);
    if (reg_err != BB_OK) return reg_err;

    bb_ws_server_set_disconnect_cb(ws_disconnect_cb, NULL);

    s_ctx.server = server;

    // Subscribe to the "log" bb_event topic (registered by bb_log_event at order 4).
    // (B) ESP-only reachability: the "log" topic is registered exclusively
    // by platform/espidf/bb_log/bb_log_event.c (ESP_PLATFORM only) — on host
    // the lookup always misses and the subscribe branch below never runs.
    // See the matching invariant comment on log_event_cb above.
    bb_event_topic_t lt;
    if (bb_event_topic_lookup("log", &lt) == BB_OK) {  // LCOV_EXCL_BR_LINE — see comment above
        // LCOV_EXCL_START — see comment above
        bb_err_t sub_err = bb_event_subscribe(lt, log_event_cb, NULL, NULL);
        if (sub_err != BB_OK) {
            bb_log_w(TAG, "log event subscribe failed: %d", (int)sub_err);
        }
        // LCOV_EXCL_STOP
    } else {
        bb_log_d(TAG, "log topic not yet registered, skipping ws subscription");
    }

    // Change-driven WS deltas (B1-589 PR-4b): observe-all so any changed
    // bb_cache key fans out an immediate delta. BB_ERR_UNSUPPORTED (Kconfig
    // BB_CACHE_REACTIVE_ENABLE=n) is expected/benign -- periodic-only push.
    bb_cache_reactive_observer_t rx_obs = {
        .key = NULL, .on_change = reactive_on_change, .ctx = NULL,
    };
    bb_err_t rx_err = bb_cache_reactive_observe(&rx_obs);
    if (rx_err != BB_OK) {
        bb_log_d(TAG, "reactive observe unavailable (%d), periodic-only push", (int)rx_err);
    }

    // Snapshot-on-connect (B1-589 PR-4b): unicast full current state to a
    // newly-connected client so a fresh dashboard doesn't wait for the next
    // tick/delta.
    bb_ws_server_set_connect_cb(ws_connect_cb, NULL);

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
    // (B) envelope_len is computed above to exactly fit this format string
    // (json_len + fixed literal overhead) — snprintf can never truncate or
    // error against a buffer sized to its own inputs; same invariant as
    // sink_ws_publish/format_push_frame above.
    if (written > 0 && (size_t)written < envelope_len) {  // LCOV_EXCL_BR_LINE — see comment above
        broadcast_filtered("log", buf, (size_t)written);
    }
    free(buf);
}

#endif // BB_SINK_WS_TESTING
