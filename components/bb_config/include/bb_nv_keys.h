#pragma once

// Single source of truth for cross-component NVS key-name strings.
// These values key persisted data on deployed boards — changing any one
// of them strands existing NVS data (the device would silently start
// reading/writing under a new, empty key on next boot).
// Companion to bb_nv_namespaces.h (namespace strings).

// bb_mqtt_client (client identifier sent to the broker)
#define BB_NV_KEY_CLIENT_ID   "client_id"

// bb_tls_creds (TLS credential PEM blobs; consumed by bb_mqtt_client under
// its namespace)
#define BB_NV_KEY_TLS_CA      "tls_ca"
#define BB_NV_KEY_TLS_CERT    "tls_cert"
#define BB_NV_KEY_TLS_KEY     "tls_key"

// bb_system reboot-reason SSOT (components/bb_system, B1-527; namespace
// BB_REBOOT_NVS_NS). Single last-reboot record, cleared on read by bb_diag
// at boot. One bb_reboot_record_encode/_decode delimited string = one key.
#define BB_REBOOT_KEY_LAST    "last"

// bb_system reboot history ring (components/bb_system, B1-527 PR-B; same
// namespace BB_REBOOT_NVS_NS as BB_REBOOT_KEY_LAST). Rolling last-8 ring,
// NOT cleared on read — accumulates across boots. One
// bb_reboot_history_encode/_decode delimited string = one key.
#define BB_REBOOT_KEY_HISTORY "history"

// bb_system boot-health counter (components/bb_system, B1-753; same
// namespace BB_REBOOT_NVS_NS). Single u8 counter, incremented on boot and
// reset on successful WiFi connect.
#define BB_REBOOT_KEY_BOOT_CNT "boot_cnt"
