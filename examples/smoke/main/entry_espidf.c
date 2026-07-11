// ESP-IDF entry shim for the unified smoke example.
//
// Bringup sequence:
//   0. bb_wifi reads bb_settings' wifi ssid/pass/has-creds accessors directly
//      for its CONNECT path (KB 805/806 — bb_wifi_creds seam collapsed).
//   0b. bb_wifi_set_ota_validated_cb() — inject bb_ota_validator's real
//      bb_ota_is_validated() before EARLY init (KB 781 wifi-split PR2:
//      bb_wifi core no longer depends on bb_ota_validator; this composes
//      the real behavior back in at the app layer).
//   1. bb_app_init_early() — EARLY tier, `bbtool codegen`-generated from
//      `// bbtool:init tier=early` markers (bb_nv_config_init,
//      bb_wifi_autoinit, etc.) -- DI DEMOLITION (KB decision #735):
//      the bb_init self-registration walker is retired from this entry
//      point (bb_init_init_early() is no longer called); generated/
//      bb_app_init.c must exist (`make smoke-codegen`) for this to link.
//      Call this EXACTLY ONCE -- bb_app_init() below is early+rest combined
//      (bb_app_init_early() + bb_app_init_rest()), so calling both
//      bb_app_init_early() and bb_app_init() here would double-fire every
//      EARLY-tier fn (bb_nv_flash_init, bb_nv_config_init,
//      bb_wifi_autoinit, ...).
//   2. bb_app_init_rest()  — PRE_HTTP tier, HTTP autostart (the
//      provides=http_server marker on bb_http_autostart_init), then the
//      REGULAR route-registration tier -- same composition bb_init_init()
//      used to drive, now generated instead of self-registered. Calls only
//      the non-EARLY tiers (EARLY already ran in step 1).
//   3. smoke_app_setup()        — app-level setup (LED, button, /ping, events).
//
// smoke_app_setup() no longer calls bb_wifi_init_sta() or bb_http_server_start()
// directly — the registry owns full bringup.

#include "bb_log.h"
#include "bb_app_init.h"
#include "bb_event.h"
#include "bb_led_info.h"
#include "bb_wifi.h"
#include "bb_ota_validator.h"
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
    // Compose bb_ota_validator's real bb_ota_is_validated() into bb_wifi's
    // cold-boot/safeguard/retry-forever gates. Must run before
    // bb_app_init_early() so it is set before the EARLY-tier bb_wifi_autoinit
    // runs (same ordering constraint as the creds provider above).
    bb_wifi_set_ota_validated_cb(bb_ota_is_validated);
    // wifi.net emit-seam wire (bb_wifi_set_emit(bb_event_emit)) is now
    // codegen-emitted into bb_app_init_early() via the provides/consumes
    // markers (bb_event.h's `// bbtool:provides key=emit_sink` +
    // bb_wifi.h's `// bbtool:init tier=early ... consumes=emit_sink`) --
    // no handwire needed here (B1-741).
    bb_app_init_early();
    bb_smoke_storage_typed_selftest();
    bb_led_register_info();
#ifdef BB_HAVE_DISPLAY_INFO
    bb_display_register_info();
#endif
    bb_app_init_rest();
    smoke_app_setup();
    bb_log_i(TAG, "smoke boot ok");
    while (1) {
        smoke_app_loop();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
