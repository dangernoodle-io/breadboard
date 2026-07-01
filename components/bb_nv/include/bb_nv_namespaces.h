#pragma once

// Single source of truth for cross-component NVS namespace strings.
// These values key persisted data on deployed boards — changing any one
// of them strands existing NVS data (the device would silently start
// reading/writing under a new, empty namespace on next boot).

#define BB_NV_CONFIG_NVS_NS   "bb_cfg"
#define BB_MQTT_NVS_NS        "bb_mqtt"
#define BB_SINK_HTTP_NVS_NS   "bb_sink_http"
#define BB_PUB_NVS_NS         "bb_pub"
