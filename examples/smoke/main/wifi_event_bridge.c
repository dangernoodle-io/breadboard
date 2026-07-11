// wifi.net publisher bridge (KB 820 PR2) -- composition glue, NOT a
// component (no bbtool:init marker, not host-built). bb_wifi stays
// bb_event-free (see the wifi.net contract block in bb_wifi.h); this file
// is the sole point of contact between the two: it registers the
// BB_WIFI_EVENT_TOPIC topic and forwards bb_wifi's net-event sink into a
// bb_event_post using bb_wifi_event_payload_build (the pure, host-tested
// builder in bb_wifi).
#include "wifi_event_bridge.h"

#include "bb_event.h"
#include "bb_log.h"
#include "bb_wifi.h"

static const char *TAG = "wifi_event_bridge";

static bb_event_topic_t s_wifi_net_topic = NULL;

static void wifi_event_bridge_sink(bb_wifi_net_event_t evt, bb_wifi_disc_reason_t reason)
{
    if (!s_wifi_net_topic) return;
    char ip[16] = {0};
    if (evt == BB_WIFI_NET_EVT_GOT_IP) {
        // Untested composition-glue edge (B1-730 coverage-blindness class):
        // on failure (netif down / get_ip_info error) both backends also
        // write the "0.0.0.0" sentinel into ip, so the return MUST be
        // checked -- ip[0] alone can't distinguish a real address from the
        // failure sentinel.
        if (bb_wifi_get_ip_str(ip, sizeof(ip)) != BB_OK) {
            ip[0] = '\0';
        }
    }
    bb_wifi_event_payload_t payload;
    bb_wifi_event_payload_build(&payload, evt, reason, ip[0] ? ip : NULL);
    bb_event_post(s_wifi_net_topic, (int32_t)evt, &payload, sizeof(payload));
}

void wifi_event_bridge_init(void)
{
    // idempotent; bb_event_autoinit calls it again later, no-op
    if (bb_event_init(NULL) != BB_OK) {
        bb_log_w(TAG, "bb_event_init failed");
    }
    if (bb_event_topic_register(BB_WIFI_EVENT_TOPIC, &s_wifi_net_topic) != BB_OK) {
        bb_log_w(TAG, "topic register failed");
    }
    // MUST be called before bb_app_init_early() -- the EARLY-tier
    // bb_wifi_autoinit connects STA and can fire the first GOT_IP before
    // this sink is registered otherwise (see bb_wifi.h
    // bb_wifi_set_net_event_sink for the full seam contract).
    bb_wifi_set_net_event_sink(wifi_event_bridge_sink);
}
