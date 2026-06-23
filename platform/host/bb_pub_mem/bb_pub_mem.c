// bb_pub_mem — telemetry source satellite: live memory metrics.
// Compiled on both host (tests) and ESP-IDF.
#include "bb_pub_mem.h"
#include "bb_pub.h"
#include "bb_board.h"
#include "bb_clock.h"
#include "bb_json.h"
#include "bb_log.h"
#include "bb_registry.h"
#include <stdbool.h>

#ifndef CONFIG_BB_PUB_MEM_AUTO_ATTACH
#define CONFIG_BB_PUB_MEM_AUTO_ATTACH 0
#endif

static const char *TAG = "bb_pub_mem";

// ---------------------------------------------------------------------------
// Sample function — called by bb_pub_tick_once for the "mem" subtopic.
// ---------------------------------------------------------------------------

static bool mem_sample(bb_json_t obj, void *ctx)
{
    (void)ctx;

    bb_json_obj_set_number(obj, "heap_internal_free",
                           (double)bb_board_heap_internal_free());
    bb_json_obj_set_number(obj, "heap_internal_min_free",
                           (double)bb_board_heap_minimum_ever());
    bb_json_obj_set_number(obj, "heap_internal_largest_block",
                           (double)bb_board_heap_internal_largest_free_block());
    // psram_free: omit when no PSRAM hardware (total == 0).
    if (bb_board_psram_total() > 0) {
        bb_json_obj_set_number(obj, "psram_free",
                               (double)bb_board_psram_free());
    }
    bb_json_obj_set_number(obj, "uptime_ms",
                           (double)bb_clock_now_ms());

    // Always publish — memory is always present.
    return true;
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

bb_err_t bb_pub_mem_register(void)
{
    bb_err_t err = bb_pub_register_source("mem", mem_sample, NULL);
    if (err == BB_OK) {
        bb_log_i(TAG, "registered mem source");
    } else if (err != BB_ERR_NO_SPACE) {
        bb_log_w(TAG, "register_source failed: %d", err);
    }
    return err;
}

// ---------------------------------------------------------------------------
// Auto-attach (PRE_HTTP tier, after bb_pub's own PRE_HTTP registration)
// ---------------------------------------------------------------------------

static bb_err_t bb_pub_mem_init(void)
{
    return bb_pub_mem_register();
}

#if CONFIG_BB_PUB_MEM_AUTO_ATTACH
BB_REGISTRY_REGISTER_PRE_HTTP(bb_pub_mem, bb_pub_mem_init);
#endif
