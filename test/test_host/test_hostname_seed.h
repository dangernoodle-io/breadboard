#pragma once

// Shared host-test helper (B1-754): seeds bb_settings' hostname field via a
// small RAM-backed "nvs" storage backend, so tests exercising consumers that
// now read hostname through bb_settings_hostname_get() (bb_pub, bb_sink_http,
// bb_sink_ws/udp/mqtt, bb_mqtt_client, bb_telemetry) can set a hostname
// without wiring up real NVS. Idempotent -- safe to call from any test's
// setUp regardless of backend-registry state left behind by other test
// files (duplicate "nvs" registration is a harmless no-op).
void bb_test_seed_hostname(const char *hostname);

// Erase the seeded hostname key so bb_settings_hostname_get() falls back to
// its has_default="" (unset) contract.
void bb_test_seed_hostname_clear(void);
