// bb_pub_info — telemetry source satellite: device info / system metrics.
// Compiled on both host (tests) and ESP-IDF.
#include "bb_pub_info.h"
#include "bb_pub.h"
#include "bb_board.h"
#include "bb_system.h"
#include "bb_clock.h"
#include "bb_diag.h"
#include "bb_json.h"
#include "bb_log.h"
#include "bb_registry.h"
#include <stdbool.h>

#ifndef CONFIG_BB_PUB_INFO_AUTO_ATTACH
#define CONFIG_BB_PUB_INFO_AUTO_ATTACH 0
#endif

static const char *TAG = "bb_pub_info";

// ---------------------------------------------------------------------------
// Sample function — called by bb_pub_tick_once for the "info" subtopic.
// ---------------------------------------------------------------------------

static bool info_sample(bb_json_t obj, void *ctx)
{
    (void)ctx;

    bb_json_obj_set_number(obj, "heap_internal_free",
                           (double)bb_board_heap_internal_free());
    bb_json_obj_set_number(obj, "heap_internal_total",
                           (double)bb_board_heap_internal_total());
    bb_json_obj_set_number(obj, "heap_internal_largest_block",
                           (double)bb_board_heap_largest_free_block());
    bb_json_obj_set_number(obj, "heap_internal_min_free",
                           (double)bb_board_heap_minimum_ever());
    bb_json_obj_set_number(obj, "psram_free",
                           (double)bb_board_psram_free());
    bb_json_obj_set_number(obj, "psram_total",
                           (double)bb_board_psram_total());
    bb_json_obj_set_number(obj, "rtc_used",
                           (double)bb_board_rtc_used());
    bb_json_obj_set_number(obj, "rtc_total",
                           (double)bb_board_rtc_total());
    bb_json_obj_set_number(obj, "flash_size",
                           (double)bb_board_get_flash_size());
    bb_json_obj_set_number(obj, "app_size",
                           (double)bb_board_get_app_size());
    bb_json_obj_set_number(obj, "wdt_resets",
                           (double)bb_diag_abnormal_reset_count());
    bb_json_obj_set_number(obj, "uptime_ms",
                           (double)bb_clock_now_ms());
    bb_json_obj_set_string(obj, "version",
                           bb_system_get_version());

    // Always publish — provides a heartbeat even without hardware HALs.
    return true;
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

bb_err_t bb_pub_info_register(void)
{
    bb_err_t err = bb_pub_register_source("info", info_sample, NULL);
    if (err == BB_OK) {
        bb_log_i(TAG, "registered info source");
    } else if (err != BB_ERR_NO_SPACE) {
        bb_log_w(TAG, "register_source failed: %d", err);
    }
    return err;
}

// ---------------------------------------------------------------------------
// Auto-attach (PRE_HTTP tier, after bb_pub's own PRE_HTTP registration)
// ---------------------------------------------------------------------------

static bb_err_t bb_pub_info_init(void)
{
    return bb_pub_info_register();
}

#if CONFIG_BB_PUB_INFO_AUTO_ATTACH
BB_REGISTRY_REGISTER_PRE_HTTP(bb_pub_info, bb_pub_info_init);
#endif
