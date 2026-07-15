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
//
// KB 820 (bb_wifi reason contract, PR1): "last_disconnect_reason" and the
// "reason_histogram"/"top_reason" fields changed from raw numeric
// esp_wifi/sentinel codes to STABLE STRING labels backed by the portable
// bb_wifi_disc_reason_t enum -- "reason_histogram" is now a string-keyed
// object of the NON-ZERO buckets (label -> count) rather than fixed
// lost_ip/egress_dead/no_ip_watchdog/top_reason_code integer fields. This is
// a BREAKING CHANGE for any consumer parsing these as integers or relying on
// the old fixed keys. See bb_wifi.h for the full enum + per-backend mapping.
#include "bb_net_health.h"
#include "bb_http.h"
#include "bb_http_server.h"
#include "bb_clock.h"
#include "bb_log.h"
#include "bb_wifi.h"
#include "bb_transport_health.h"

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

    // bb_transport_health LIVE snapshot (B1-518 PR2, OBSERVE-ONLY): built at
    // request time, not cached in bb_net_health_status_t — variable-length,
    // so it does not go through the fixed-size evaluator snapshot/cache path
    // the other fields above use. Always emitted, even when empty (an empty
    // array is meaningful: "nothing registered yet"), unlike the "gw" object
    // below which is omitted entirely when the probe is disabled.
    {
        bb_transport_health_snapshot_t th[BB_TRANSPORT_HEALTH_MAX_SLOTS];
        size_t th_n = bb_transport_health_snapshot_all(th, BB_TRANSPORT_HEALTH_MAX_SLOTS);
        bb_http_resp_json_obj_set_arr_begin(&obj, "transports");
        for (size_t i = 0; i < th_n; i++) {
            bb_http_resp_json_obj_set_obj_begin(&obj, NULL);
            bb_http_resp_json_obj_set_str (&obj, "name", th[i].name);
            bb_http_resp_json_obj_set_str (&obj, "cls",
                th[i].cls == BB_TRANSPORT_AUTHORITATIVE ? "authoritative" : "inferred");
            bb_http_resp_json_obj_set_bool(&obj, "enabled",    th[i].enabled);
            bb_http_resp_json_obj_set_bool(&obj, "failing",    th[i].failing);
            bb_http_resp_json_obj_set_int (&obj, "last_ok_ms", (int64_t)th[i].last_ok_ms);
            bb_http_resp_json_obj_set_int (&obj, "fail_count", (int64_t)th[i].fail_count);
            if (th[i].cls == BB_TRANSPORT_INFERRED) {
                bb_http_resp_json_obj_set_int(&obj, "last_rx_ms", (int64_t)th[i].last_rx_ms);
                bb_http_resp_json_obj_set_int(&obj, "rx_count",   (int64_t)th[i].rx_count);
            }
            bb_http_resp_json_obj_set_obj_end(&obj);
        }
        bb_http_resp_json_obj_set_arr_end(&obj);
    }

    bb_net_health_status_t snap;
    if (bb_net_health_get_status(&snap) == BB_OK) {
        // B1-486 finding #4: no_ip_recoveries is folded into the same 5s
        // evaluator snapshot as lost_ip_recoveries/egress_dead_recoveries
        // (rather than a separate live bb_wifi_get_no_ip_count() call) so
        // recovery_count sums point-in-time-consistent operands.
        bb_http_resp_json_obj_set_int(&obj, "no_ip_recoveries",       (int64_t)snap.no_ip_recoveries);
        bb_http_resp_json_obj_set_int(&obj, "rssi",                   (int64_t)snap.rssi);
        bb_http_resp_json_obj_set_int(&obj, "disc_age_s",             (int64_t)snap.disc_age_s);
        bb_http_resp_json_obj_set_str(&obj, "last_disconnect_reason",
            bb_wifi_disc_reason_str((bb_wifi_disc_reason_t)snap.last_disconnect_reason));
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
        bb_http_resp_json_obj_set_str (&obj, "net_mode",   bb_wifi_mode_str(snap.net_mode));
        bb_http_resp_json_obj_set_bool(&obj, "associated", snap.associated);
        bb_http_resp_json_obj_set_bool(&obj, "has_ip",     snap.has_ip);

        // Egress-recovery SSOT verdict (B1-518 PR3, OBSERVE-ONLY): derived
        // from net_mode + gw-probe + transport-health each evaluator cycle
        // (bb_net_health_classify_egress). No recovery action is wired to
        // this field.
        bb_http_resp_json_obj_set_str(&obj, "egress_state", bb_egress_state_str(snap.egress_state));

        bb_http_resp_json_obj_set_obj_begin(&obj, "mqtt");
        bb_http_resp_json_obj_set_int(&obj, "reconnect_count", (int64_t)snap.mqtt_reconnect_count);
        bb_http_resp_json_obj_set_int(&obj, "disc_age_s",      (int64_t)snap.mqtt_disc_age_s);
        bb_http_resp_json_obj_set_int(&obj, "disc_reason",     (int64_t)snap.mqtt_disc_reason);
        bb_http_resp_json_obj_set_int(&obj, "tls_fail",        (int64_t)snap.mqtt_tls_fail);
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

    // Reason histogram: a string-keyed object of the NON-ZERO buckets
    // (label -> count) plus the top non-injected standard reason. Superset
    // of the reason_histogram removed from /api/wifi and the "wifi"
    // telemetry topic.
    uint16_t hist[BB_WIFI_DISC_COUNT];
    bb_wifi_get_reason_histogram(hist, BB_WIFI_DISC_COUNT);
    uint16_t top_count = 0;
    bb_wifi_disc_reason_t top_reason = bb_wifi_reason_histogram_top(hist, &top_count);

    bb_http_resp_json_obj_set_obj_begin(&obj, "reason_histogram");
    for (int i = 0; i < BB_WIFI_DISC_COUNT; i++) {
        if (hist[i] == 0) continue;
        bb_http_resp_json_obj_set_int(&obj, bb_wifi_disc_reason_str((bb_wifi_disc_reason_t)i),
                                       (int64_t)hist[i]);
    }
    bb_http_resp_json_obj_set_str(&obj, "top_reason",       bb_wifi_disc_reason_str(top_reason));
    bb_http_resp_json_obj_set_int(&obj, "top_reason_count", (int64_t)top_count);
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
      "\"transports\":{\"type\":\"array\",\"items\":{\"type\":\"object\","
      "\"properties\":{"
      "\"name\":{\"type\":\"string\"},"
      "\"cls\":{\"type\":\"string\"},"
      "\"enabled\":{\"type\":\"boolean\"},"
      "\"failing\":{\"type\":\"boolean\"},"
      "\"last_ok_ms\":{\"type\":\"integer\"},"
      "\"fail_count\":{\"type\":\"integer\"},"
      "\"last_rx_ms\":{\"type\":\"integer\"},"
      "\"rx_count\":{\"type\":\"integer\"}},"
      "\"required\":[\"name\",\"cls\",\"enabled\",\"failing\"]}},"
      "\"no_ip_recoveries\":{\"type\":\"integer\"},"
      "\"rssi\":{\"type\":\"integer\"},"
      "\"disc_age_s\":{\"type\":\"integer\"},"
      "\"last_disconnect_reason\":{\"type\":\"string\"},"
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
      "\"egress_state\":{\"type\":\"string\"},"
      "\"mqtt\":{\"type\":\"object\",\"properties\":{"
      "\"reconnect_count\":{\"type\":\"integer\"},"
      "\"disc_age_s\":{\"type\":\"integer\"},"
      "\"disc_reason\":{\"type\":\"integer\"},"
      "\"tls_fail\":{\"type\":\"integer\"}}},"
      "\"gw\":{\"type\":\"object\",\"properties\":{"
      "\"gw_reachable\":{\"type\":\"boolean\"},"
      "\"gw_fail_streak\":{\"type\":\"integer\"},"
      "\"gw_dead_count\":{\"type\":\"integer\"},"
      "\"gw_probe_age_s\":{\"type\":\"integer\"}}},"
      "\"reason_histogram\":{\"type\":\"object\","
      "\"additionalProperties\":{\"type\":\"integer\"},"
      "\"properties\":{"
      "\"top_reason\":{\"type\":\"string\"},"
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

bb_err_t bb_net_health_routes_init(bb_http_handle_t server)
{
    if (!server) return BB_ERR_INVALID_ARG;
    bb_err_t err = bb_http_register_described_route(server, &s_diag_net_route);
    if (err != BB_OK) return err;
    bb_log_i(TAG, "registered /api/diag/net");
    return BB_OK;
}
