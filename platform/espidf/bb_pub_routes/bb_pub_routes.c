// bb_pub_routes — GET /api/pub publisher status route.
//
// Reports whether the telemetry publisher is actually emitting, distinct from
// whether a transport (MQTT) is connected. Exposes bb_pub's own state so the
// UI can show a meaningful "publishing" indicator.
#include "bb_pub_routes.h"
#include "bb_pub.h"
#include "bb_clock.h"
#include "bb_http.h"
#include "bb_log.h"
#include "bb_registry.h"

#include <stdbool.h>
#include <stdint.h>

static const char *TAG = "bb_pub_routes";

// Kconfig defaults for host builds (provided by Kconfig on ESP-IDF).
#ifndef CONFIG_BB_PUB_INTERVAL_MS
#define CONFIG_BB_PUB_INTERVAL_MS 10000
#endif
#ifndef CONFIG_BB_PUB_TOPIC_PREFIX
#define CONFIG_BB_PUB_TOPIC_PREFIX "metrics"
#endif

// ---------------------------------------------------------------------------
// GET /api/pub
// ---------------------------------------------------------------------------

static bb_err_t pub_get_handler(bb_http_request_t *req)
{
    bb_pub_status_t st;
    bb_pub_get_status(&st);

    uint32_t now_ms = bb_clock_now_ms();
    int32_t  age_ms = -1;
    if (st.published_ever) {
        age_ms = (int32_t)(now_ms - st.last_publish_ms);
    }

    bb_http_json_obj_stream_t obj;
    bb_err_t err = bb_http_resp_json_obj_begin(req, &obj);
    if (err != BB_OK) return err;
    bb_http_resp_json_obj_set_int(&obj, "interval_ms",       CONFIG_BB_PUB_INTERVAL_MS);
    bb_http_resp_json_obj_set_str(&obj, "topic_prefix",      CONFIG_BB_PUB_TOPIC_PREFIX);
    bb_http_resp_json_obj_set_int(&obj, "source_count",      st.source_count);
    bb_http_resp_json_obj_set_int(&obj, "sink_count",        st.sink_count);
    bb_http_resp_json_obj_set_bool(&obj, "last_publish_ok",  st.last_publish_ok);
    bb_http_resp_json_obj_set_int(&obj, "last_publish_age_ms", age_ms);
    bb_http_resp_json_obj_set_bool(&obj, "published_ever",   st.published_ever);
    return bb_http_resp_json_obj_end(&obj);
}

// ---------------------------------------------------------------------------
// Route descriptor
// ---------------------------------------------------------------------------

static const bb_route_response_t s_pub_get_responses[] = {
    { 200, "application/json",
      "{\"type\":\"object\","
      "\"properties\":{"
      "\"interval_ms\":{\"type\":\"integer\"},"
      "\"topic_prefix\":{\"type\":\"string\"},"
      "\"source_count\":{\"type\":\"integer\"},"
      "\"sink_count\":{\"type\":\"integer\"},"
      "\"last_publish_ok\":{\"type\":\"boolean\"},"
      "\"last_publish_age_ms\":{\"type\":\"integer\","
        "\"description\":\"-1 when never published\"},"
      "\"published_ever\":{\"type\":\"boolean\"}},"
      "\"required\":[\"interval_ms\",\"topic_prefix\","
                   "\"source_count\",\"sink_count\","
                   "\"last_publish_ok\",\"last_publish_age_ms\","
                   "\"published_ever\"]}",
      "Publisher status" },
    { 0 },
};

static const bb_route_t s_pub_get_route = {
    .method    = BB_HTTP_GET,
    .path      = "/api/pub",
    .tag       = "pub",
    .summary   = "Get telemetry publisher status",
    .responses = s_pub_get_responses,
    .handler   = pub_get_handler,
};

// ---------------------------------------------------------------------------
// Init (regular-tier)
// ---------------------------------------------------------------------------

bb_err_t bb_pub_routes_init(bb_http_handle_t server)
{
    if (!server) return BB_ERR_INVALID_ARG;

    bb_err_t rc = bb_http_register_described_route(server, &s_pub_get_route);
    if (rc != BB_OK) return rc;

    bb_log_i(TAG, "pub routes registered");
    return BB_OK;
}

#if CONFIG_BB_PUB_ROUTES_AUTOREGISTER
BB_REGISTRY_REGISTER_N(bb_pub_routes, bb_pub_routes_init, 6);
#endif
