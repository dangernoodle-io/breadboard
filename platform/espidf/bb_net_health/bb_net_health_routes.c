// ESP-IDF route handler for bb_net_health — self-registers GET /api/diag/net
// so network diagnostic counters are owned by the component that produces
// them, mirroring how bb_partition_routes owns /api/diag/partitions and
// bb_event_routes owns /api/diag/events.
//
// Sources: bb_net_health snapshot, bb_clock_now_ms, bb_http handler counts
// (TA-505; relocated from bb_diag_routes.c under B1-456).
#include "bb_net_health.h"
#include "bb_http.h"
#include "bb_clock.h"
#include "bb_init.h"
#include "bb_log.h"

static const char *TAG = "bb_net_health_routes";

// GET /api/diag/net — network diagnostic counters.
static bb_err_t diag_net_handler(bb_http_request_t *req)
{
    bb_http_json_obj_stream_t obj;
    bb_err_t err = bb_http_resp_json_obj_begin(req, &obj);
    if (err != BB_OK) return err;

    bb_http_resp_json_obj_set_int(&obj, "uptime_ms",           (int64_t)bb_clock_now_ms());
    bb_http_resp_json_obj_set_int(&obj, "http_handler_count",  (int64_t)bb_http_route_handler_count());
    bb_http_resp_json_obj_set_int(&obj, "http_handler_cap",    (int64_t)bb_http_route_handler_cap());

    bb_net_health_status_t snap;
    if (bb_net_health_get_status(&snap) == BB_OK) {
        bb_http_resp_json_obj_set_int(&obj, "rssi",                   (int64_t)snap.rssi);
        bb_http_resp_json_obj_set_int(&obj, "disc_age_s",             (int64_t)snap.disc_age_s);
        bb_http_resp_json_obj_set_int(&obj, "last_disconnect_reason", (int64_t)snap.last_disconnect_reason);
        bb_http_resp_json_obj_set_int(&obj, "lost_ip_recoveries",     (int64_t)snap.lost_ip_recoveries);
        bb_http_resp_json_obj_set_int(&obj, "lost_ip_age_s",          (int64_t)snap.lost_ip_age_s);
        bb_http_resp_json_obj_set_int(&obj, "egress_dead_recoveries", (int64_t)snap.egress_dead_recoveries);

        bb_http_resp_json_obj_set_obj_begin(&obj, "mqtt");
        bb_http_resp_json_obj_set_int(&obj, "reconnect_count", (int64_t)snap.mqtt_reconnect_count);
        bb_http_resp_json_obj_set_int(&obj, "disc_age_s",      (int64_t)snap.mqtt_disc_age_s);
        bb_http_resp_json_obj_set_int(&obj, "disc_reason",     (int64_t)snap.mqtt_disc_reason);
        bb_http_resp_json_obj_set_int(&obj, "tls_fail",        (int64_t)snap.mqtt_tls_fail);
        bb_http_resp_json_obj_set_obj_end(&obj);

        bb_http_resp_json_obj_set_obj_begin(&obj, "http");
        bb_http_resp_json_obj_set_int(&obj, "consec_failures", (int64_t)snap.http_consec_failures);
        bb_http_resp_json_obj_set_int(&obj, "tls_fail",        (int64_t)snap.http_tls_fail);
        bb_http_resp_json_obj_set_int(&obj, "last_status",     (int64_t)snap.http_last_status);
        bb_http_resp_json_obj_set_obj_end(&obj);
    }

    return bb_http_resp_json_obj_end(&obj);
}

static const bb_route_response_t s_diag_net_responses[] = {
    { 200, "application/json",
      "{\"type\":\"object\","
      "\"properties\":{"
      "\"uptime_ms\":{\"type\":\"integer\"},"
      "\"http_handler_count\":{\"type\":\"integer\"},"
      "\"http_handler_cap\":{\"type\":\"integer\"},"
      "\"rssi\":{\"type\":\"integer\"},"
      "\"disc_age_s\":{\"type\":\"integer\"},"
      "\"last_disconnect_reason\":{\"type\":\"integer\"},"
      "\"lost_ip_recoveries\":{\"type\":\"integer\"},"
      "\"lost_ip_age_s\":{\"type\":\"integer\"},"
      "\"egress_dead_recoveries\":{\"type\":\"integer\"},"
      "\"mqtt\":{\"type\":\"object\",\"properties\":{"
      "\"reconnect_count\":{\"type\":\"integer\"},"
      "\"disc_age_s\":{\"type\":\"integer\"},"
      "\"disc_reason\":{\"type\":\"integer\"},"
      "\"tls_fail\":{\"type\":\"integer\"}}},"
      "\"http\":{\"type\":\"object\",\"properties\":{"
      "\"consec_failures\":{\"type\":\"integer\"},"
      "\"tls_fail\":{\"type\":\"integer\"},"
      "\"last_status\":{\"type\":\"integer\"}}}},"
      "\"required\":[\"uptime_ms\"]}",
      "network diagnostic counters relocated from /api/info and /api/health" },
    { 0 },
};

static const bb_route_t s_diag_net_route = {
    .method    = BB_HTTP_GET,
    .path      = "/api/diag/net",
    .tag       = "diag",
    .summary   = "Network diagnostic counters (uptime, rssi, reconnect counts, tls failures)",
    .responses = s_diag_net_responses,
    .handler   = diag_net_handler,
};

static bb_err_t bb_net_health_routes_init(bb_http_handle_t server)
{
    if (!server) return BB_ERR_INVALID_ARG;
    bb_err_t err = bb_http_register_described_route(server, &s_diag_net_route);
    if (err != BB_OK) return err;
    bb_log_i(TAG, "registered /api/diag/net");
    return BB_OK;
}

#if CONFIG_BB_NET_HEALTH_ROUTES_AUTOREGISTER
BB_INIT_REGISTER(bb_net_health_routes, bb_net_health_routes_init);
#endif
