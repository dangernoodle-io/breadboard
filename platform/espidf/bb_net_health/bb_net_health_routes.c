// ESP-IDF route handler for bb_net_health — self-registers GET /api/diag/net
// so network diagnostic counters are owned by the component that produces
// them, mirroring how bb_partition_routes owns /api/diag/partitions and
// bb_event_routes owns /api/diag/events.
//
// Sources: bb_net_health snapshot, bb_clock_now_ms, bb_http handler counts
// (TA-505; relocated from bb_diag_routes.c under B1-456).
//
// B1-486: /api/diag/net is the single source of truth for wifi recovery
// counters. no_ip_recoveries, recovery_count (sum of no_ip/lost_ip/egress_dead),
// and reason_histogram were previously duplicated on GET /api/wifi and the
// "wifi" telemetry topic; no_ip_recoveries is folded into the same 5s
// evaluator snapshot as lost_ip_recoveries/egress_dead_recoveries (via
// bb_net_health_status_t) so recovery_count sums point-in-time-consistent
// operands, and top_reason is computed via bb_wifi's public
// bb_wifi_reason_histogram_top() (this component already PRIV_REQUIRES
// bb_wifi) so /api/diag/net is a strict superset of what /api/wifi used to
// expose.
//
// B1-518 PR3: a "gw" object (gw_reachable, gw_fail_streak, gw_dead_count,
// gw_probe_age_s) is emitted ONLY when the gateway-probe worker
// (CONFIG_BB_WIFI_GW_PROBE_ENABLE) has completed at least one probe. OBSERVE
// ONLY — no recovery/classifier logic; see bb_wifi_get_gateway_status().
#include "bb_net_health.h"
#include "bb_http.h"
#include "bb_clock.h"
#include "bb_init.h"
#include "bb_log.h"
#include "bb_wifi.h"

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
        // B1-486 finding #4: no_ip_recoveries is folded into the same 5s
        // evaluator snapshot as lost_ip_recoveries/egress_dead_recoveries
        // (rather than a separate live bb_wifi_get_no_ip_count() call) so
        // recovery_count sums point-in-time-consistent operands.
        bb_http_resp_json_obj_set_int(&obj, "no_ip_recoveries",       (int64_t)snap.no_ip_recoveries);
        bb_http_resp_json_obj_set_int(&obj, "rssi",                   (int64_t)snap.rssi);
        bb_http_resp_json_obj_set_int(&obj, "disc_age_s",             (int64_t)snap.disc_age_s);
        bb_http_resp_json_obj_set_int(&obj, "last_disconnect_reason", (int64_t)snap.last_disconnect_reason);
        bb_http_resp_json_obj_set_int(&obj, "lost_ip_recoveries",     (int64_t)snap.lost_ip_recoveries);
        bb_http_resp_json_obj_set_int(&obj, "lost_ip_age_s",          (int64_t)snap.lost_ip_age_s);
        bb_http_resp_json_obj_set_int(&obj, "egress_dead_recoveries", (int64_t)snap.egress_dead_recoveries);
        bb_http_resp_json_obj_set_int(&obj, "recovery_count",
            (int64_t)(snap.no_ip_recoveries + snap.lost_ip_recoveries + snap.egress_dead_recoveries));
        // B1-497: OBSERVE-ONLY roam/BSSID-change counter — not summed into
        // recovery_count (no recovery action is associated with a roam).
        bb_http_resp_json_obj_set_int(&obj, "roam_count", (int64_t)snap.roam_count);
        bb_http_resp_json_obj_set_int(&obj, "roam_age_s", (int64_t)snap.roam_age_s);
        // Duration of the most recently ended connected session (OBSERVE-ONLY;
        // captured in the same evaluator snapshot as the other counters above).
        bb_http_resp_json_obj_set_int(&obj, "last_session_s", (int64_t)snap.last_session_s);

        // WiFi discrimination mode (OBSERVE-ONLY, no recovery action wired):
        // distinguishes no-IP-while-associated from not-associated-at-all.
        bb_http_resp_json_obj_set_str (&obj, "net_mode",   bb_net_mode_str(snap.net_mode));
        bb_http_resp_json_obj_set_bool(&obj, "associated", snap.associated);
        bb_http_resp_json_obj_set_bool(&obj, "has_ip",     snap.has_ip);

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

        // Gateway-probe status (B1-518 PR3, OBSERVE-ONLY): omitted entirely
        // when the probe worker has never completed a probe (disabled via
        // CONFIG_BB_WIFI_GW_PROBE_ENABLE=n, or not yet started) so consumers
        // never see fabricated zero values.
        if (snap.gw_available) {
            uint64_t now_ms = bb_clock_now_ms64();
            uint32_t age_s = (snap.last_gw_probe_ms > 0 && now_ms >= snap.last_gw_probe_ms)
                ? (uint32_t)((now_ms - snap.last_gw_probe_ms) / 1000ULL)
                : 0;
            bb_http_resp_json_obj_set_obj_begin(&obj, "gw");
            bb_http_resp_json_obj_set_bool(&obj, "gw_reachable",   snap.gw_reachable);
            bb_http_resp_json_obj_set_int (&obj, "gw_fail_streak", (int64_t)snap.gw_fail_streak);
            bb_http_resp_json_obj_set_int (&obj, "gw_dead_count",  (int64_t)snap.gw_dead_count);
            bb_http_resp_json_obj_set_int (&obj, "gw_probe_age_s", (int64_t)age_s);
            bb_http_resp_json_obj_set_obj_end(&obj);
        }
    }

    // Compact reason histogram: the three breadboard sentinel buckets + top
    // non-sentinel standard reason. Superset of the reason_histogram removed
    // from /api/wifi and the "wifi" telemetry topic.
    uint16_t hist[256];
    bb_wifi_get_reason_histogram(hist, 256);
    uint16_t top_count = 0;
    uint8_t  top_code  = bb_wifi_reason_histogram_top(hist, &top_count);

    bb_http_resp_json_obj_set_obj_begin(&obj, "reason_histogram");
    bb_http_resp_json_obj_set_int(&obj, "lost_ip",         (int64_t)hist[BB_WIFI_REASON_BB_LOST_IP]);
    bb_http_resp_json_obj_set_int(&obj, "egress_dead",     (int64_t)hist[BB_WIFI_REASON_BB_EGRESS_DEAD]);
    bb_http_resp_json_obj_set_int(&obj, "no_ip_watchdog",  (int64_t)hist[BB_WIFI_REASON_BB_NO_IP_WATCHDOG]);
    bb_http_resp_json_obj_set_int(&obj, "top_reason_code", (int64_t)top_code);
    bb_http_resp_json_obj_set_int(&obj, "top_reason_count",(int64_t)top_count);
    bb_http_resp_json_obj_set_obj_end(&obj);

    return bb_http_resp_json_obj_end(&obj);
}

static const bb_route_response_t s_diag_net_responses[] = {
    { 200, "application/json",
      "{\"type\":\"object\","
      "\"properties\":{"
      "\"uptime_ms\":{\"type\":\"integer\"},"
      "\"http_handler_count\":{\"type\":\"integer\"},"
      "\"http_handler_cap\":{\"type\":\"integer\"},"
      "\"no_ip_recoveries\":{\"type\":\"integer\"},"
      "\"rssi\":{\"type\":\"integer\"},"
      "\"disc_age_s\":{\"type\":\"integer\"},"
      "\"last_disconnect_reason\":{\"type\":\"integer\"},"
      "\"lost_ip_recoveries\":{\"type\":\"integer\"},"
      "\"lost_ip_age_s\":{\"type\":\"integer\"},"
      "\"egress_dead_recoveries\":{\"type\":\"integer\"},"
      "\"recovery_count\":{\"type\":\"integer\"},"
      "\"roam_count\":{\"type\":\"integer\"},"
      "\"roam_age_s\":{\"type\":\"integer\"},"
      "\"last_session_s\":{\"type\":\"integer\"},"
      "\"net_mode\":{\"type\":\"string\"},"
      "\"associated\":{\"type\":\"boolean\"},"
      "\"has_ip\":{\"type\":\"boolean\"},"
      "\"mqtt\":{\"type\":\"object\",\"properties\":{"
      "\"reconnect_count\":{\"type\":\"integer\"},"
      "\"disc_age_s\":{\"type\":\"integer\"},"
      "\"disc_reason\":{\"type\":\"integer\"},"
      "\"tls_fail\":{\"type\":\"integer\"}}},"
      "\"http\":{\"type\":\"object\",\"properties\":{"
      "\"consec_failures\":{\"type\":\"integer\"},"
      "\"tls_fail\":{\"type\":\"integer\"},"
      "\"last_status\":{\"type\":\"integer\"}}},"
      "\"gw\":{\"type\":\"object\",\"properties\":{"
      "\"gw_reachable\":{\"type\":\"boolean\"},"
      "\"gw_fail_streak\":{\"type\":\"integer\"},"
      "\"gw_dead_count\":{\"type\":\"integer\"},"
      "\"gw_probe_age_s\":{\"type\":\"integer\"}}},"
      "\"reason_histogram\":{\"type\":\"object\",\"properties\":{"
      "\"lost_ip\":{\"type\":\"integer\"},"
      "\"egress_dead\":{\"type\":\"integer\"},"
      "\"no_ip_watchdog\":{\"type\":\"integer\"},"
      "\"top_reason_code\":{\"type\":\"integer\"},"
      "\"top_reason_count\":{\"type\":\"integer\"}}}},"
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
