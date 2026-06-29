// bb_pub_info — telemetry source satellite: device info / system metrics.
// Compiled on both host (tests) and ESP-IDF.
//
// Migration (telemetry-ssot): uses bb_pub_register_telemetry so the snapshot
// is gathered into bb_cache once per tick; sinks-only (BB_PUB_TELEM_SINKS,
// no SSE — info is a sink-only topic).  REST reads the SAME memoized
// serialization via bb_cache_get_serialized.  The sample-time timestamp is
// stamped into the snapshot at gather time.
//
// Snap size: ~360 bytes — exceeds the 256-byte default BB_PUB_TELEM_SNAP_MAX.
// The Kconfig default has been raised to 512 in components/bb_pub/Kconfig.
#include "bb_pub_info.h"
#include "bb_pub.h"
#include "bb_board.h"
#include "bb_system.h"
#include "bb_clock.h"
#include "bb_diag.h"
#include "bb_json.h"
#include "bb_log.h"
#include "bb_ntp.h"
#include "bb_openapi.h"
#include "bb_registry.h"
// ota_ready (heap-readiness for the OTA TLS handshake) is emitted only on boards
// that run a heap-guarded OTA TLS path — the runtime pull worker
// (BB_OTA_PULL_AUTOREGISTER) or the boot-mode on-demand check
// (BB_OTA_BOOT_STATUS_HTTP). Gated so boot-only boards don't link bb_ota_pull
// just for this field. Keep in sync with bb_info.c + both CMakeLists.
#if defined(ESP_PLATFORM) && \
    ((defined(CONFIG_BB_OTA_PULL_AUTOREGISTER) && CONFIG_BB_OTA_PULL_AUTOREGISTER) || \
     (defined(CONFIG_BB_OTA_BOOT_STATUS_HTTP) && CONFIG_BB_OTA_BOOT_STATUS_HTTP))
#define BB_PUB_INFO_EMIT_OTA_READY 1
#include "bb_ota_pull.h"
#endif
#include <stdbool.h>
#include <string.h>
#include <time.h>

#ifndef CONFIG_BB_PUB_INFO_AUTO_ATTACH
#define CONFIG_BB_PUB_INFO_AUTO_ATTACH 0
#endif

static const char *TAG = "bb_pub_info";

// ---------------------------------------------------------------------------
// Snapshot struct — captured once per tick under the tick lock.
// Size: ~360 bytes — requires CONFIG_BB_PUB_TELEM_SNAP_MAX >= 512.
// The Kconfig default has been raised to 512 in components/bb_pub/Kconfig.
// ---------------------------------------------------------------------------

typedef struct {
    // Heap / memory metrics
    size_t heap_internal_free;
    size_t heap_internal_total;
    size_t heap_internal_largest_block;
    size_t heap_internal_min_free;
    size_t psram_free;
    size_t psram_total;
    size_t rtc_used;
    size_t rtc_total;
    size_t rtc_free;
    size_t dram_static_bytes;
    size_t flash_size;
    size_t app_size;

    // Health counters
    uint32_t wdt_resets;

    // Identity strings
    char version[32];
    char board[32];
    char chip_model[16];
    char mac[18];
    char reset_reason[16];

    // OTA / time
    bool    ota_validated;
#if BB_PUB_INFO_EMIT_OTA_READY
    bool    ota_ready;
#endif
    bool    time_valid;
    int64_t boot_epoch_s;
    char    time_source[8]; // "sntp" or "none"

    // has_psram: whether to emit psram fields
    bool    has_psram;

    // Sample-time timestamp
    int64_t ts_ms;
} bb_info_snap_t;

// ---------------------------------------------------------------------------
// Gather — fills snap from live system state; called under lock.
// Always returns true (info is a heartbeat source).
// ---------------------------------------------------------------------------

static bool info_gather(void *snap_buf, void *ctx)
{
    (void)ctx;
    bb_info_snap_t *s = snap_buf;
    memset(s, 0, sizeof(*s));

    s->heap_internal_free          = bb_board_heap_internal_free();
    s->heap_internal_total         = bb_board_heap_internal_total();
    s->heap_internal_largest_block = bb_board_heap_internal_largest_free_block();
    s->heap_internal_min_free      = bb_board_heap_internal_minimum_ever();
    s->psram_free                  = bb_board_psram_free();
    s->psram_total                 = bb_board_psram_total();
    s->rtc_used                    = bb_board_rtc_used();
    s->rtc_total                   = bb_board_rtc_total();
    s->dram_static_bytes           = bb_board_dram_static_bytes();
    s->flash_size                  = bb_board_get_flash_size();
    s->app_size                    = bb_board_get_app_size();
    s->wdt_resets                  = (uint32_t)bb_diag_abnormal_reset_count();
    s->has_psram                   = (s->psram_total > 0);
    s->rtc_free = (s->rtc_total >= s->rtc_used) ? (s->rtc_total - s->rtc_used) : 0;

    const char *ver = bb_system_get_version();
    if (ver) strncpy(s->version, ver, sizeof(s->version) - 1);

    {
        bb_board_info_t bi;
        if (bb_board_get_info(&bi) == BB_OK) {
            strncpy(s->board,      bi.board,      sizeof(s->board)      - 1);
            strncpy(s->chip_model, bi.chip_model, sizeof(s->chip_model) - 1);
            s->ota_validated = bi.ota_validated;
        }
    }

    bb_board_get_mac(s->mac, sizeof(s->mac));
    bb_board_get_reset_reason(s->reset_reason, sizeof(s->reset_reason));

#if BB_PUB_INFO_EMIT_OTA_READY
    s->ota_ready = bb_ota_pull_heap_ready();
#endif

    s->time_valid    = false;
    s->boot_epoch_s  = 0;
    strncpy(s->time_source, "none", sizeof(s->time_source) - 1);
    if (bb_ntp_is_synced()) {
        time_t now = time(NULL);
        if (now >= (time_t)1704067200LL) {
            s->time_valid   = true;
            int64_t uptime_s = (int64_t)bb_clock_now_ms() / 1000;
            s->boot_epoch_s = (int64_t)now - uptime_s;
            strncpy(s->time_source, "sntp", sizeof(s->time_source) - 1);
        }
    }

    s->ts_ms = (int64_t)bb_clock_now_ms64();
    return true;
}

// ---------------------------------------------------------------------------
// Serialize — called by bb_cache to build JSON from the frozen snapshot.
// Mirrors info_sample logic but reads fully from snap (SSOT guarantee).
// ---------------------------------------------------------------------------

static void info_serialize(bb_json_t obj, const void *snap_raw)
{
    const bb_info_snap_t *s = snap_raw;

    bb_json_obj_set_number(obj, "heap_internal_free",          (double)s->heap_internal_free);
    bb_json_obj_set_number(obj, "heap_internal_total",         (double)s->heap_internal_total);
    bb_json_obj_set_number(obj, "heap_internal_largest_block", (double)s->heap_internal_largest_block);
    bb_json_obj_set_number(obj, "heap_internal_min_free",      (double)s->heap_internal_min_free);
    if (s->has_psram) {
        bb_json_obj_set_number(obj, "psram_free",  (double)s->psram_free);
        bb_json_obj_set_number(obj, "psram_total", (double)s->psram_total);
    }
    bb_json_obj_set_number(obj, "rtc_used",          (double)s->rtc_used);
    bb_json_obj_set_number(obj, "rtc_total",         (double)s->rtc_total);
    bb_json_obj_set_number(obj, "dram_static_bytes", (double)s->dram_static_bytes);
    bb_json_obj_set_number(obj, "flash_size",        (double)s->flash_size);
    bb_json_obj_set_number(obj, "app_size",          (double)s->app_size);
    bb_json_obj_set_number(obj, "wdt_resets",        (double)s->wdt_resets);
    bb_json_obj_set_string(obj, "version",           s->version);
    bb_json_obj_set_string(obj, "board",             s->board);
    bb_json_obj_set_string(obj, "chip_model",        s->chip_model);
    bb_json_obj_set_string(obj, "mac",               s->mac);
    bb_json_obj_set_string(obj, "reset_reason",      s->reset_reason);
    bb_json_obj_set_bool  (obj, "ota_validated",     s->ota_validated);
#if BB_PUB_INFO_EMIT_OTA_READY
    bb_json_obj_set_bool  (obj, "ota_ready",         s->ota_ready);
#endif
    bb_json_obj_set_bool  (obj, "time_valid",        s->time_valid);
    bb_json_obj_set_number(obj, "boot_epoch_s",      (double)s->boot_epoch_s);
    bb_json_obj_set_string(obj, "time_source",       s->time_source);
    bb_json_obj_set_number(obj, "rtc_free",          (double)s->rtc_free);
    bb_json_obj_set_int   (obj, "ts_ms",             s->ts_ms);
}

// ---------------------------------------------------------------------------
// Schema + Registration
// ---------------------------------------------------------------------------

// InfoTelemetry is a sink-only topic (no SSE); sse_topic=NULL.
static const char k_info_telemetry_schema[] =
    "{\"title\":\"InfoTelemetry\",\"type\":\"object\","
    "\"properties\":{"
    "\"heap_internal_free\":{\"type\":\"number\"},"
    "\"heap_internal_total\":{\"type\":\"number\"},"
    "\"heap_internal_largest_block\":{\"type\":\"number\"},"
    "\"heap_internal_min_free\":{\"type\":\"number\"},"
    "\"psram_free\":{\"type\":\"number\"},"
    "\"psram_total\":{\"type\":\"number\"},"
    "\"rtc_used\":{\"type\":\"number\"},"
    "\"rtc_total\":{\"type\":\"number\"},"
    "\"dram_static_bytes\":{\"type\":\"number\"},"
    "\"flash_size\":{\"type\":\"number\"},"
    "\"app_size\":{\"type\":\"number\"},"
    "\"wdt_resets\":{\"type\":\"number\"},"
    "\"version\":{\"type\":\"string\"},"
    "\"board\":{\"type\":\"string\"},"
    "\"chip_model\":{\"type\":\"string\"},"
    "\"mac\":{\"type\":\"string\"},"
    "\"reset_reason\":{\"type\":\"string\"},"
    "\"ota_validated\":{\"type\":\"boolean\"},"
    "\"time_valid\":{\"type\":\"boolean\"},"
    "\"boot_epoch_s\":{\"type\":\"number\"},"
    "\"time_source\":{\"type\":\"string\"},"
    "\"rtc_free\":{\"type\":\"number\"},"
    "\"ts_ms\":{\"type\":\"integer\"}},"
    "\"required\":[\"heap_internal_free\",\"heap_internal_total\","
    "\"heap_internal_largest_block\",\"heap_internal_min_free\","
    "\"rtc_used\",\"rtc_total\",\"dram_static_bytes\",\"flash_size\","
    "\"app_size\",\"wdt_resets\",\"version\",\"board\",\"chip_model\","
    "\"mac\",\"reset_reason\",\"ota_validated\",\"time_valid\","
    "\"boot_epoch_s\",\"time_source\",\"rtc_free\",\"ts_ms\"]}";

bb_err_t bb_pub_info_register(void)
{
    bb_pub_telemetry_cfg_t cfg = {
        .topic     = "info",
        .gather    = info_gather,
        .serialize = info_serialize,
        .snap_size = sizeof(bb_info_snap_t),
        .flags     = BB_PUB_TELEM_SINKS,  // sinks-only: no SSE for info
        .ctx       = NULL,
    };

    bb_openapi_register_schema("InfoTelemetry", k_info_telemetry_schema, NULL);

    bb_err_t err = bb_pub_register_telemetry(&cfg);
    if (err == BB_OK) {
        bb_log_i(TAG, "registered info telemetry source (snap_size=%zu)", sizeof(bb_info_snap_t));
    } else if (err != BB_ERR_NO_SPACE) {
        bb_log_w(TAG, "register_telemetry failed: %d", err);
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
