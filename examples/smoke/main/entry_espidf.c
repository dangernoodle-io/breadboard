// ESP-IDF entry shim for the unified smoke example.
//
// Bringup sequence:
//   0. bb_wifi_set_creds_provider() — inject bb_settings' default wifi-creds
//      provider before EARLY init (see KB 781 wifi-PR3 seam adoption).
//   0b. bb_wifi_set_ota_validated_cb() — inject bb_ota_validator's real
//      bb_ota_is_validated() before EARLY init (KB 781 wifi-split PR2:
//      bb_wifi core no longer depends on bb_ota_validator; this composes
//      the real behavior back in at the app layer).
//   1. bb_init_init_early() — EARLY tier: bb_nv_config_init, bb_wifi_init_sta
//      (auto-registered via BB_WIFI_AUTOREGISTER).
//   2. bb_init_init()       — PRE_HTTP tier, then HTTP autostart
//      (BB_HTTP_AUTOSTART), then regular route registration tier.
//   3. smoke_app_setup()        — app-level setup (LED, button, /ping, events).
//
// smoke_app_setup() no longer calls bb_wifi_init_sta() or bb_http_server_start()
// directly — the registry owns full bringup.

#include "bb_log.h"
#include "bb_init.h"
#include "bb_led_info.h"
#include "bb_wifi.h"
#include "bb_ota_validator.h"
#include "bb_settings.h"
#include "smoke_app.h"
#include "storage_typed_selftest.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if __has_include("bb_display_info.h")
#include "bb_display_info.h"
#define BB_HAVE_DISPLAY_INFO 1
#endif

static const char *TAG = "smoke";

void app_main(void)
{
    // Compose bb_settings' default wifi-creds provider into bb_wifi's CONNECT
    // path. Must run before bb_init_init_early() so it is set before the
    // EARLY-tier bb_wifi_autoinit runs.
    bb_wifi_set_creds_provider(bb_settings_wifi_creds_provider(), bb_settings_wifi_creds_ctx());
    // Compose bb_ota_validator's real bb_ota_is_validated() into bb_wifi's
    // cold-boot/safeguard/retry-forever gates. Must run before
    // bb_init_init_early() so it is set before the EARLY-tier bb_wifi_autoinit
    // runs (same ordering constraint as the creds provider above).
    bb_wifi_set_ota_validated_cb(bb_ota_is_validated);
    bb_init_init_early();
    bb_smoke_storage_typed_selftest();
    bb_led_register_info();
#ifdef BB_HAVE_DISPLAY_INFO
    bb_display_register_info();
#endif
    bb_init_init();
    smoke_app_setup();
    bb_log_i(TAG, "smoke boot ok");
    while (1) {
        smoke_app_loop();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
