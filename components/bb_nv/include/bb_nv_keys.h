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

// bb_net_health egress-recovery ACT gate (components/bb_net_health, B1-518
// PR4; namespace BB_NET_HEALTH_EGRESS_ACT_NVS_NS). The reboot-rate-limit
// state (last_reboot_s + ring) is packed into ONE delimited string via
// bb_net_health_reboot_state_encode/_decode and persisted under a single
// key — one bb_nv_set_str call = one NVS commit, instead of one commit per
// scalar field (was: last_reboot_s + ring_head + ring_count + 10 ring_N
// entries = 13 commits per persist).
#define BB_NET_HEALTH_EGRESS_ACT_KEY_STATE       "state"

// bb_system reboot-reason SSOT (components/bb_system, B1-527; namespace
// BB_REBOOT_NVS_NS). Single last-reboot record, cleared on read by bb_diag
// at boot. One bb_reboot_record_encode/_decode delimited string = one key.
#define BB_REBOOT_KEY_LAST    "last"

// bb_system reboot history ring (components/bb_system, B1-527 PR-B; same
// namespace BB_REBOOT_NVS_NS as BB_REBOOT_KEY_LAST). Rolling last-8 ring,
// NOT cleared on read — accumulates across boots. One
// bb_reboot_history_encode/_decode delimited string = one key.
#define BB_REBOOT_KEY_HISTORY "history"
