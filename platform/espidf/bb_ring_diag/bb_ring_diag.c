// ESP-IDF route handler for bb_ring_diag — registers GET /api/diag/rings.
// Mirrors bb_event_routes_espidf's GET /api/diag/events: each component
// registers its own diag route rather than bb_diag core owning it.
//
// Snapshot-first / copy-out pattern (MANDATORY per bb_ring_registry.h's
// foreach contract): bb_ring_registry_foreach holds its internal lock across
// the entire call, including every callback invocation — this is what
// prevents a concurrent bb_ring_destroy() from freeing a ring mid-read
// (use-after-free). That means the callback here must NOT perform any I/O
// (httpd_resp_send_chunk fires mid-iteration once the streaming JSON buffer
// fills) — a slow client would otherwise hold the registry lock and stall
// every bb_ring_create/destroy/count call fleet-wide. Instead: copy each
// ring's name + scalar stats into a fixed-size stack array while the lock is
// held (bounded, allocation-free, no I/O), then stream the JSON response
// from that snapshot AFTER bb_ring_registry_foreach returns (lock released).
#include "bb_ring_diag.h"
#include "bb_ring.h"
#include "bb_ring_registry.h"
#include "bb_http.h"
#include "bb_http_server.h"
#include "bb_log.h"
#include "bb_str.h"

#include <stdint.h>
#include <string.h>

static const char *TAG = "bb_ring_diag";

typedef struct {
    char     name[BB_RING_NAME_MAX];
    uint32_t count;
    uint32_t capacity;
    uint32_t dropped;
    uint32_t truncated;
    uint32_t bytes_used;
} ring_snapshot_t;

typedef struct {
    ring_snapshot_t *snap;
    int              n;
    int              max;
} snapshot_ctx_t;

// Runs under bb_ring_registry's lock — copy only, no I/O, no allocation.
static void rings_snapshot_cb(const char *name, bb_ring_t r, void *ctx)
{
    snapshot_ctx_t *sc = (snapshot_ctx_t *)ctx;
    if (sc->n >= sc->max) {
        return;  // registry is bounded by BB_RING_REGISTRY_MAX; defensive only
    }
    ring_snapshot_t *s = &sc->snap[sc->n];
    bb_strlcpy(s->name, name ? name : "", sizeof(s->name));
    // size_t -> uint32_t narrowing is safe here: these are diagnostic ring
    // counts/dropped/truncated (bounded by BB_RING_REGISTRY_MAX-sized rings)
    // and cumulative byte totals — none realistically approach 4G on an
    // embedded target, so truncation is not a practical concern.
    s->count      = (uint32_t)bb_ring_count(r);
    s->capacity   = (uint32_t)bb_ring_capacity(r);
    s->dropped    = (uint32_t)bb_ring_dropped(r);
    s->truncated  = (uint32_t)bb_ring_truncated(r);
    s->bytes_used = (uint32_t)bb_ring_bytes_used(r);
    sc->n++;
}

static bb_err_t rings_get_handler(bb_http_request_t *req)
{
    // Snapshot phase — bb_ring_registry's lock is held for the duration of
    // this one call, but rings_snapshot_cb does only bounded memcpys, so the
    // hold time is a handful of memcpys, not a client's network RTT.
    ring_snapshot_t snap[CONFIG_BB_RING_REGISTRY_MAX];
    snapshot_ctx_t sc = { .snap = snap, .n = 0, .max = CONFIG_BB_RING_REGISTRY_MAX };
    bb_ring_registry_foreach(rings_snapshot_cb, &sc);

    // Stream phase — lock is released; every httpd_resp_send_chunk below
    // (direct or via bb_http_resp_json_obj_* buffering) happens with no
    // bb_ring_registry lock held.
    bb_http_json_obj_stream_t obj;
    bb_err_t err = bb_http_resp_json_obj_begin(req, &obj);
    if (err != BB_OK) return err;

    bb_http_resp_json_obj_set_int(&obj, "count",             (int64_t)sc.n);
    bb_http_resp_json_obj_set_int(&obj, "registry_capacity", (int64_t)CONFIG_BB_RING_REGISTRY_MAX);

    bb_http_resp_json_obj_set_arr_begin(&obj, "rings");
    for (int i = 0; i < sc.n; i++) {
        bb_http_resp_json_obj_set_obj_begin(&obj, NULL);
        bb_http_resp_json_obj_set_str(&obj, "name",       snap[i].name);
        bb_http_resp_json_obj_set_int(&obj, "count",      (int64_t)snap[i].count);
        bb_http_resp_json_obj_set_int(&obj, "capacity",   (int64_t)snap[i].capacity);
        bb_http_resp_json_obj_set_int(&obj, "dropped",    (int64_t)snap[i].dropped);
        bb_http_resp_json_obj_set_int(&obj, "truncated",  (int64_t)snap[i].truncated);
        bb_http_resp_json_obj_set_int(&obj, "bytes_used", (int64_t)snap[i].bytes_used);
        bb_http_resp_json_obj_set_obj_end(&obj);
    }
    bb_http_resp_json_obj_set_arr_end(&obj);

    return bb_http_resp_json_obj_end(&obj);
}

static const bb_route_response_t s_rings_get_responses[] = {
    { 200, "application/json",
      "{\"type\":\"object\","
      "\"properties\":{"
      "\"count\":{\"type\":\"integer\"},"
      "\"registry_capacity\":{\"type\":\"integer\"},"
      "\"rings\":{\"type\":\"array\",\"items\":{\"type\":\"object\","
      "\"properties\":{"
      "\"name\":{\"type\":\"string\"},"
      "\"count\":{\"type\":\"integer\"},"
      "\"capacity\":{\"type\":\"integer\"},"
      "\"dropped\":{\"type\":\"integer\"},"
      "\"truncated\":{\"type\":\"integer\"},"
      "\"bytes_used\":{\"type\":\"integer\"}},"
      "\"required\":[\"name\",\"count\",\"capacity\"]}}},"
      "\"required\":[\"count\",\"registry_capacity\",\"rings\"]}",
      "every live bb_ring_t registered via bb_ring_registry" },
    { 0 },
};

static const bb_route_t s_rings_get_route = {
    .method    = BB_HTTP_GET,
    .path      = "/api/diag/rings",
    .tag       = "diag",
    .summary   = "List every live bb_ring_t (name, count, capacity, dropped, truncated, bytes_used)",
    .responses = s_rings_get_responses,
    .handler   = rings_get_handler,
};

bb_err_t bb_ring_diag_init(bb_http_handle_t server)
{
    if (!server) return BB_ERR_INVALID_ARG;
    bb_err_t err = bb_http_register_described_route(server, &s_rings_get_route);
    if (err != BB_OK) return err;
    bb_log_i(TAG, "registered /api/diag/rings");
    return BB_OK;
}

bb_err_t bb_ring_diag_reserve_routes(void)
{
    bb_http_reserve_routes(1);  // GET /api/diag/rings
    return BB_OK;
}
