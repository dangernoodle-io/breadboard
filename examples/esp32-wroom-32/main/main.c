#include "bb_log.h"
#include "bb_wifi.h"
#include "bb_http.h"
#include "bb_registry.h"

static const char *TAG = "smoke";

// Runtime-mode smoke. Provisioning mode is mutually exclusive with OTA on a
// single boot, so bb_prov is intentionally not exercised here — file a separate
// prov-mode smoke if coverage of that path is needed on classic ESP32.
void app_main(void) {
    bb_registry_init_early();

    bb_wifi_ensure_netif();
    // Try to bring up STA if creds are provisioned in NVS. Returns BB_ERR_TIMEOUT
    // on failure; smoke is OK either way — HTTP server still starts.
    (void)bb_wifi_init_sta();

    bb_http_server_start();

    bb_registry_init();

    bb_log_i(TAG, "smoke boot ok");
}
