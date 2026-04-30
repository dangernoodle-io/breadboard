// ESP-IDF entry shim for the unified smoke example.
//
// smoke_app_setup() calls bb_nv_config_init(), bb_wifi_init_sta(),
// bb_http_server_start(), and registers /ping. bb_registry_init_early()
// also triggers bb_nv_config_init via BB_REGISTRY_REGISTER_EARLY — the
// double-call is safe: nvs_flash_init() returns ESP_ERR_INVALID_STATE on
// re-init (non-fatal), and bb_nv_config_init simply re-reads the config
// namespace, which is idempotent.

#include "bb_log.h"
#include "bb_registry.h"
#include "smoke_app.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "smoke";

void app_main(void)
{
    bb_registry_init_early();
    smoke_app_setup();
    bb_registry_init();
    bb_log_i(TAG, "smoke boot ok");
    while (1) {
        smoke_app_loop();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
