// ESP-IDF entry shim for the unified smoke example.
//
// Bringup sequence:
//   1. bb_registry_init_early() — EARLY tier: bb_nv_config_init, bb_wifi_init_sta
//      (auto-registered via BB_WIFI_AUTOREGISTER).
//   2. bb_registry_init()       — PRE_HTTP tier, then HTTP autostart
//      (BB_HTTP_AUTOSTART), then regular route registration tier.
//   3. smoke_app_setup()        — app-level setup (LED, button, /ping, events).
//
// smoke_app_setup() no longer calls bb_wifi_init_sta() or bb_http_server_start()
// directly — the registry owns full bringup.

#include "bb_log.h"
#include "bb_registry.h"
#include "smoke_app.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "smoke";

void app_main(void)
{
    bb_registry_init_early();
    bb_registry_init();
    smoke_app_setup();
    bb_log_i(TAG, "smoke boot ok");
    while (1) {
        smoke_app_loop();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
