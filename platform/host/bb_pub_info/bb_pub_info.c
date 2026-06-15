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
#include "bb_ntp.h"
#include "bb_ota_validator.h"
#include "bb_registry.h"
#include <stdbool.h>
#include <string.h>
#include <time.h>

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
                           (double)bb_board_heap_internal_largest_free_block());
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

    // Static identity fields: board, chip_model, mac (fleet identification).
    char board[32];
    char chip_model[16];
    char mac[18];
    board[0]      = '\0';
    chip_model[0] = '\0';
    mac[0]        = '\0';
    {
        bb_board_info_t bi;
        if (bb_board_get_info(&bi) == BB_OK) {
            strncpy(board,      bi.board,      sizeof(board)      - 1);
            strncpy(chip_model, bi.chip_model, sizeof(chip_model) - 1);
        }
    }
    bb_board_get_mac(mac, sizeof(mac));
    bb_json_obj_set_string(obj, "board",      board);
    bb_json_obj_set_string(obj, "chip_model", chip_model);
    bb_json_obj_set_string(obj, "mac",        mac);

    // reset_reason: string boot cause ("power-on", "software", "panic", ...)
    char reset_reason[16];
    bb_board_get_reset_reason(reset_reason, sizeof(reset_reason));
    bb_json_obj_set_string(obj, "reset_reason", reset_reason);

    // ota_validated: true unless running OTA slot is PENDING_VERIFY
    bb_json_obj_set_bool(obj, "ota_validated", bb_ota_is_validated());

    // rtc_free: derived from rtc_total - rtc_used (RTC slow memory free bytes)
    size_t rtc_total = bb_board_rtc_total();
    size_t rtc_used  = bb_board_rtc_used();
    size_t rtc_free  = (rtc_total >= rtc_used) ? (rtc_total - rtc_used) : 0;
    bb_json_obj_set_number(obj, "rtc_free", (double)rtc_free);

    // RTC clock fields: time_valid, epoch_s, time_source
    bool   time_valid  = false;
    int64_t epoch_s    = 0;
    const char *time_source = "none";
    if (bb_ntp_is_synced()) {
        time_t now = time(NULL);
        // Sanity-check: year >= 2024 (unix epoch >= 1704067200)
        if (now >= (time_t)1704067200LL) {
            time_valid  = true;
            epoch_s     = (int64_t)now;
            time_source = "sntp";
        }
    }
    bb_json_obj_set_bool  (obj, "time_valid",   time_valid);
    bb_json_obj_set_number(obj, "epoch_s",      (double)epoch_s);
    bb_json_obj_set_string(obj, "time_source",  time_source);

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
