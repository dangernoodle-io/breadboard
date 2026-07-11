#pragma once

// wifi.net publisher bridge (KB 820 PR2) -- composition glue, not a
// component. Bridges bb_wifi's net-event sink (bb_wifi_set_net_event_sink)
// to a bb_event publish on BB_WIFI_EVENT_TOPIC ("wifi.net"). See
// wifi_event_bridge.c for the wiring.
void wifi_event_bridge_init(void);
