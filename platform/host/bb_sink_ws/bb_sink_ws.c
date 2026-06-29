// bb_sink_ws — bb_pub sink that forwards telemetry payloads to WebSocket clients.
// Host implementation: compiled for native tests; no FreeRTOS, no bb_log_stream.
//
// Subscription filtering
// ----------------------
// Clients send a TEXT frame: {"sub":["telemetry","events","logs",...]}
// This replaces the client's entire subscription set.  On the next publish
// only channels matching the subscription are delivered.
//
// Matching rules (checked in order):
//   1. Exact match:  client subscribed to "power"  → receives ch="power"
//   2. Coarse group: client subscribed to "telemetry" → receives any ch
//      that maps to a bb_pub telemetry subtopic (mining, pool, info, wifi,
//      fan, power, thermal, …).  All bb_pub subtopics that are NOT "events"
//      or "logs" are considered part of the "telemetry" group.
//   3. "events" group: receives ch values that come from the events bus
//      (subtopic == "events").
//   4. "logs" group: receives ch="logs" frames.
//   5. If the client has NO subscriptions yet (e.g., just connected and has
//      not sent a sub frame), it receives NOTHING (subscription required).
//
// Per-client subscription state
// -----------------------------
// Keyed by socket fd (0..BB_WEBSOCKET_MAX_FD-1).  A fixed-size slot array
// avoids dynamic allocation and survives connections/disconnections.

#include "bb_sink_ws.h"
#include "bb_websocket.h"
#include "bb_log.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "bb_sink_ws";

// ---------------------------------------------------------------------------
// Subscription table
// ---------------------------------------------------------------------------

// Max subscription strings per client.
#define BB_SINK_WS_MAX_SUBS 8
// Max length of a single subscription string (channel name).
#define BB_SINK_WS_SUB_MAX_LEN 32

typedef struct {
    bool active;                                 // slot is in use
    char subs[BB_SINK_WS_MAX_SUBS][BB_SINK_WS_SUB_MAX_LEN];
    int  sub_count;
} client_sub_t;

static client_sub_t s_clients[BB_WEBSOCKET_MAX_FD];
static bool s_suspended = false;

// Clear all subscription state for a client slot.
static void client_sub_clear(int fd)
{
    if (fd < 0 || fd >= BB_WEBSOCKET_MAX_FD) return;
    memset(&s_clients[fd], 0, sizeof(s_clients[fd]));
}

// Parse {"sub":["a","b",...]} and populate the client slot.
// Returns true if parsed successfully (even if array is empty).
// Returns false and leaves state unchanged on parse failure.
static bool parse_sub_frame(int fd, const char *payload, size_t len)
{
    if (fd < 0 || fd >= BB_WEBSOCKET_MAX_FD) return false;

    // Must start with {"sub":[
    const char *NEEDLE = "\"sub\":[";
    const char *p = payload ? (const char *)payload : "";
    const char *end = p + len;

    // Find "sub":[
    while (p < end && *p != '"') p++;
    const char *found = NULL;
    size_t needle_len = strlen(NEEDLE);
    for (const char *q = p; q + needle_len <= end; q++) {
        if (memcmp(q, NEEDLE, needle_len) == 0) {
            found = q + needle_len;
            break;
        }
    }
    if (!found) return false;

    // Parse quoted strings until ']'
    char new_subs[BB_SINK_WS_MAX_SUBS][BB_SINK_WS_SUB_MAX_LEN];
    int  new_count = 0;
    const char *r = found;
    while (r < end && *r != ']' && new_count < BB_SINK_WS_MAX_SUBS) {
        // Advance to '"'
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

    // Commit parsed state
    client_sub_t *slot = &s_clients[fd];
    slot->active    = true;
    slot->sub_count = new_count;
    for (int i = 0; i < new_count; i++) {
        memcpy(slot->subs[i], new_subs[i], BB_SINK_WS_SUB_MAX_LEN);
    }
    return true;
}

// Check if a client (fd) is subscribed to a given channel (ch).
//
// Matching logic:
//   - Exact name match: sub == ch
//   - Coarse group "telemetry": matches any ch that is NOT "events" and NOT "logs"
//   - Coarse group "events":    matches ch == "events"
//   - Coarse group "logs":      matches ch == "logs"
static bool client_subscribed(int fd, const char *ch)
{
    if (fd < 0 || fd >= BB_WEBSOCKET_MAX_FD) return false;
    const client_sub_t *slot = &s_clients[fd];
    if (!slot->active || slot->sub_count == 0) return false;

    for (int i = 0; i < slot->sub_count; i++) {
        const char *sub = slot->subs[i];
        // Exact match
        if (strcmp(sub, ch) == 0) return true;
        // Coarse group: "telemetry" covers anything that is not events or logs
        if (strcmp(sub, "telemetry") == 0 &&
            strcmp(ch,  "events")    != 0 &&
            strcmp(ch,  "logs")      != 0) return true;
        // Coarse group: "events" covers ch="events"
        if (strcmp(sub, "events") == 0 && strcmp(ch, "events") == 0) return true;
        // Coarse group: "logs" covers ch="logs"
        if (strcmp(sub, "logs") == 0 && strcmp(ch, "logs") == 0) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// WebSocket handler and route descriptor
// ---------------------------------------------------------------------------

static bb_err_t ws_handler(bb_http_request_t *req, const bb_websocket_frame_t *frame)
{
    // Only process TEXT frames carrying subscription commands.
    if (!frame || frame->type != BB_WS_TYPE_TEXT || !frame->payload || frame->len == 0) {
        return BB_OK;
    }

    int fd = bb_websocket_req_fd(req);
    // Attempt to parse {"sub":[...]}; ignore malformed frames silently.
    parse_sub_frame(fd, (const char *)frame->payload, frame->len);
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

// Broadcast `buf` (len bytes, ch = channel name) only to clients subscribed
// to that channel.
static bb_err_t broadcast_filtered(const char *ch,
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
        if (!client_subscribed(fd, ch)) continue;
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
                                const char *payload, int len)
{
    (void)ctx;

    const char *subtopic = extract_subtopic(topic);

    // Build {"ch":"<subtopic>","data":<payload>}
    // overhead: {"ch":"(7) + subtopic + ","data":(9) + payload + }(1) + NUL(1) = 18
    size_t subtopic_len = strlen(subtopic);
    size_t envelope_len = subtopic_len + (size_t)len + 18;
    char *buf = (char *)_sink_malloc(envelope_len);
    if (!buf) {
        bb_log_w(TAG, "publish: malloc failed for envelope");
        return BB_ERR_NO_SPACE;
    }

    int written = snprintf(buf, envelope_len, "{\"ch\":\"%s\",\"data\":%.*s}",
                           subtopic, len, payload);
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
// Public API
// ---------------------------------------------------------------------------

bb_err_t bb_sink_ws_init(bb_http_handle_t server, bb_pub_sink_t *out)
{
    if (!out) return BB_ERR_INVALID_ARG;

    bb_err_t reg_err = bb_websocket_register_described_endpoint(server, "/ws", ws_handler, &s_ws_route);
    if (reg_err != BB_OK) return reg_err;

    s_ctx.server = server;

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
    s_malloc_fn = NULL;
    s_suspended = false;
}

// Inject a log line as if the logs pump had drained it — broadcasts
// {"ch":"logs","data":"<line>"} to all subscribed clients.
bb_err_t bb_sink_ws_host_inject_log_line(const char *line)
{
    if (!line) return BB_ERR_INVALID_ARG;
    size_t line_len = strlen(line);
    // {"ch":"logs","data":"<line>"}
    // overhead: 20 + line + 2 quotes + NUL
    size_t buf_len = line_len + 24;
    char *buf = (char *)_sink_malloc(buf_len);
    if (!buf) return BB_ERR_NO_SPACE;
    int written = snprintf(buf, buf_len, "{\"ch\":\"logs\",\"data\":\"%s\"}", line);
    if (written < 0 || (size_t)written >= buf_len) {
        free(buf);
        return BB_ERR_NO_SPACE;
    }
    bb_err_t err = broadcast_filtered("logs", buf, (size_t)written);
    free(buf);
    return err;
}

#endif // BB_SINK_WS_TESTING
