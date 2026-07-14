#pragma once

// Shared host-test helper (B1-754): seeds bb_settings' hostname field via a
// small RAM-backed "nvs" storage backend, so tests exercising consumers that
// read hostname through bb_settings_hostname_get() (e.g. bb_mqtt_client) can
// set a hostname without wiring up real NVS. Idempotent -- safe to call from
// any test's
// setUp regardless of backend-registry state left behind by other test
// files (duplicate "nvs" registration is a harmless no-op).
void bb_test_seed_hostname(const char *hostname);

// Erase the seeded hostname key so bb_settings_hostname_get() falls back to
// its has_default="" (unset) contract.
void bb_test_seed_hostname_clear(void);
