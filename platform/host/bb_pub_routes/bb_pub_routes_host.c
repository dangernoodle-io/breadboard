// bb_pub_routes host twin — provides test hooks for GET /api/pub.
//
// The actual GET handler logic is compiled from the espidf source on the host
// (same pattern as bb_fan_routes, bb_power_routes, bb_mqtt_routes).
// This file only provides the test-hook surface.
#include "bb_pub_routes.h"
#include "bb_pub.h"
#include "bb_clock.h"
#include "bb_http.h"

#include <stdbool.h>
#include <stdint.h>

// Kconfig defaults for host builds.
#ifndef CONFIG_BB_PUB_INTERVAL_MS
#define CONFIG_BB_PUB_INTERVAL_MS 10000
#endif
#ifndef CONFIG_BB_PUB_TOPIC_PREFIX
#define CONFIG_BB_PUB_TOPIC_PREFIX "metrics"
#endif

// ---------------------------------------------------------------------------
// GET handler (mirrors espidf)
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
    bb_http_resp_json_obj_set_int(&obj, "interval_ms",         CONFIG_BB_PUB_INTERVAL_MS);
    bb_http_resp_json_obj_set_str(&obj, "topic_prefix",        CONFIG_BB_PUB_TOPIC_PREFIX);
    bb_http_resp_json_obj_set_int(&obj, "source_count",        st.source_count);
    bb_http_resp_json_obj_set_int(&obj, "sink_count",          st.sink_count);
    bb_http_resp_json_obj_set_bool(&obj, "last_publish_ok",    st.last_publish_ok);
    bb_http_resp_json_obj_set_int(&obj, "last_publish_age_ms", age_ms);
    bb_http_resp_json_obj_set_bool(&obj, "published_ever",     st.published_ever);
    return bb_http_resp_json_obj_end(&obj);
}

// ---------------------------------------------------------------------------
// Route descriptors
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

bb_err_t bb_pub_routes_init(bb_http_handle_t server)
{
    (void)server;
    // Host: route dispatch goes through test_http_utils; just return OK.
    return BB_OK;
}

// ---------------------------------------------------------------------------
// Test hooks
// ---------------------------------------------------------------------------

#ifdef BB_PUB_ROUTES_TESTING

void bb_pub_routes_reset_for_test(void)
{
    bb_pub_test_reset();
}

bb_err_t bb_pub_routes_get_handler_for_test(bb_http_request_t *req)
{
    return pub_get_handler(req);
}

#endif /* BB_PUB_ROUTES_TESTING */
