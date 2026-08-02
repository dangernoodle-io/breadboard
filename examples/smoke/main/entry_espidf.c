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
//      `// bbtool:init tier=early` markers (bb_settings_creds_boot_init,
//      bb_wifi_autoinit, etc.) -- DI DEMOLITION (KB decision #735):
//      the bb_init self-registration walker is retired from this entry
//      point (bb_init_init_early() is no longer called); generated/
//      bb_app_init.c must exist (`make smoke-codegen`) for this to link.
//      Call this EXACTLY ONCE -- bb_app_init() below is early+rest combined
//      (bb_app_init_early() + bb_app_init_rest()), so calling both
//      bb_app_init_early() and bb_app_init() here would double-fire every
//      EARLY-tier fn (bb_storage_nvs_register, bb_settings_creds_boot_init,
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
#include "bb_wifi.h"
#include "bb_ota_validator.h"
#include "bb_openapi.h"
#include "bb_data_http.h"
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
    // wifi.net emit-seam wire: B1-1045 dissolved the bb_event provider that
    // used to satisfy bb_wifi.h's `// bbtool:init tier=early ...
    // consumes=emit_sink` marker (B1-741) -- an unmatched consumes= key is a
    // soft no-op in codegen (not a hard error), so smoke simply leaves the
    // emit seam unwired for now. Repointing it to bb_lifecycle is smoke's
    // rehab (B1-1051), out of scope for this survival-only strip; see
    // examples/floor/main/floor_app.c for the real B1-1045 cutover wiring.
    bb_err_t early_err = bb_app_init_early();
    if (early_err != BB_OK) {
        bb_log_e(TAG, "bb_app_init_early failed (%d) -- one or more EARLY-tier "
                 "components did not fully initialize; continuing boot", (int)early_err);
    }
    bb_smoke_storage_typed_selftest();
#ifdef BB_HAVE_DISPLAY_INFO
    bb_display_register_info();
#endif
    // B1-1220 PR2 seam: any producer describing via bb_data_http_describe()
    // (bb_log_event's "log" topic, migrated off the legacy
    // bb_openapi_register_topic_schema() path) only reaches /api/openapi.json
    // if this is wired. The slot is read lazily, per-request, inside
    // openapi_handler() -> bb_openapi_emit_stream() -- bb_openapi_init()
    // never snapshots it -- so this only needs to land before the first
    // GET /api/openapi.json, not strictly before bb_app_init_rest() below;
    // placing it here is simply the earliest, safest spot.
    bb_openapi_set_topic_source_fn(bb_data_http_describe_foreach);
    // Composition-root failure posture (B1-1354): a failed component init
    // (e.g. bb_data table exhaustion, B1-1353) must never vanish silently --
    // previously this return was discarded outright, so a component could
    // lose its HTTP routes and periodic worker with nothing in the boot log
    // to show for it. bb_app_init_rest() already aggregates first-error
    // across every REGULAR/PRE_HTTP-tier init call (generated/bb_app_init.c);
    // logging loudly here, then continuing to boot, was chosen over aborting/
    // rebooting: every one of those init calls is itself non-fatal by
    // construction (first-error semantics, no early exit -- a failure in one
    // does not stop the rest from running), so a top-level abort would
    // contradict that established per-call contract and turn one optional
    // component's failure into a boot loop for the whole board. A board that
    // comes up with a missing feature and a loud log line is recoverable and
    // diagnosable (OTA a fix, or fix root cause and reboot); a board that
    // reboot-loops because one non-critical component (e.g. an update-check
    // route) failed to bind is a field brick. If a future component's
    // failure is genuinely load-bearing for safe operation, that call site
    // is the place to add a targeted fatal path -- not a blanket abort here.
    bb_err_t rest_err = bb_app_init_rest();
    if (rest_err != BB_OK) {
        bb_log_e(TAG, "bb_app_init_rest failed (%d) -- one or more components "
                 "did not fully initialize; continuing boot", (int)rest_err);
    }
    smoke_app_setup();
    bb_log_i(TAG, "smoke boot ok");
    while (1) {
        smoke_app_loop();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
