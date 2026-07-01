#pragma once

// Single source of truth for cross-component NVS key-name strings.
// These values key persisted data on deployed boards — changing any one
// of them strands existing NVS data (the device would silently start
// reading/writing under a new, empty key on next boot).
// Companion to bb_nv_namespaces.h (namespace strings).

// bb_sink_http (components/bb_sink_http, bb_sink_http_telemetry)
#define BB_NV_KEY_HEADERS     "headers"
#define BB_NV_KEY_PATH_TMPL   "path_tmpl"

// bb_mqtt / bb_sink_http shared (client identifier sent to broker/endpoint)
#define BB_NV_KEY_CLIENT_ID   "client_id"

// bb_tls_creds (TLS credential PEM blobs; consumed by bb_mqtt and bb_sink_http
// under their respective namespaces)
#define BB_NV_KEY_TLS_CA      "tls_ca"
#define BB_NV_KEY_TLS_CERT    "tls_cert"
#define BB_NV_KEY_TLS_KEY     "tls_key"

// bb_pub (components/bb_pub)
#define BB_PUB_NVS_KEY_INTERVAL "interval_ms"
#define BB_PUB_NVS_KEY_ENABLED  "enabled"
