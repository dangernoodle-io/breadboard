// bb_pub_info — telemetry source satellite: dynamic device runtime metrics.
// Compiled on both host (tests) and ESP-IDF.
//
// Migration (telemetry-ssot): uses bb_pub_register_telemetry so the snapshot
// is gathered into bb_cache once per tick; sinks-only (BB_PUB_TELEM_SINKS,
// no SSE — info is a sink-only topic).  REST reads the SAME memoized
// serialization via bb_cache_get_serialized.  bb_cache owns the envelope's
// ts_ms (B1-570 PR-3) — this source no longer stamps or emits its own
// timestamp.
//
// TA-505 (PR-2): static device-identity fields (version, board, chip_model,
// mac, flash_size, app_size, dram_static_bytes, reset_reason, boot_epoch_s,
// time_source, rtc_used, rtc_total) have been moved to the meta topic
// (bb_pub_telemetry_host.c).  rtc_free is dropped (derived from rtc_total -
// rtc_used).  This file carries dynamic runtime metrics only.
//
// Snap size: ~160 bytes — fits the 512-byte CONFIG_BB_PUB_TELEM_SNAP_MAX.
#include "bb_pub_info.h"
#include "bb_mem.h"
#include "bb_pub.h"
#include "bb_pub_defaults.h"
#include "bb_board.h"
#include "bb_diag.h"
#include "bb_json.h"
#include "bb_log.h"
#include "bb_ntp.h"
#include "bb_openapi.h"
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

static const char *TAG = "bb_pub_info";

// ---------------------------------------------------------------------------
// Snapshot struct — captured once per tick under the tick lock.
// Carries dynamic runtime metrics only; static identity fields live in
// the meta topic (bb_pub_telemetry_host.c).  Size: ~160 bytes.
// ---------------------------------------------------------------------------

typedef struct {
    // Heap / memory metrics (dynamic)
    size_t heap_internal_free;
    size_t heap_internal_total;
    size_t heap_internal_largest_block;
    size_t heap_internal_min_free;
    size_t psram_free;
    size_t psram_total;

    // Health counters (dynamic)
    uint32_t wdt_resets;

    // OTA / time state (dynamic)
    bool    ota_validated;
#if BB_PUB_INFO_EMIT_OTA_READY
    bool    ota_ready;
#endif
    bool    time_valid;

    // has_psram: whether to emit psram fields
    bool    has_psram;

    // bb_mem facade accounting (zero when BB_MEM_STATS_ENABLE is off)
    size_t   bb_mem_out;   // outstanding_bytes at gather time
    size_t   bb_mem_peak;  // peak_outstanding ever
    uint32_t bb_mem_fail;  // cumulative alloc_fail count
} bb_info_snap_t;

// Compile-time guard: info snap must fit within the static scratch buffer.
// Mirrors the _meta_snap_size_check in bb_pub_telemetry_host.c.
typedef char _info_snap_size_check[
    sizeof(bb_info_snap_t) <= CONFIG_BB_PUB_TELEM_SNAP_MAX ? 1 : -1];

// ---------------------------------------------------------------------------
// Gather — fills snap from live system state; called under lock.
// Always returns true (info is a heartbeat source).
// ---------------------------------------------------------------------------

static bool info_gather(void *snap_buf, void *ctx)
{
    (void)ctx;
    bb_info_snap_t *s = snap_buf;
    memset(s, 0, sizeof(*s));

    // Dynamic heap metrics.
    s->heap_internal_free          = bb_board_heap_internal_free();
    s->heap_internal_total         = bb_board_heap_internal_total();
    s->heap_internal_largest_block = bb_board_heap_internal_largest_free_block();
    s->heap_internal_min_free      = bb_board_heap_internal_minimum_ever();
    s->psram_free                  = bb_board_psram_free();
    s->psram_total                 = bb_board_psram_total();
    s->wdt_resets                  = (uint32_t)bb_diag_abnormal_reset_count();
    s->has_psram                   = (s->psram_total > 0);

    // ota_validated is a runtime state (changes after post-boot mark-valid).
    {
        bb_board_info_t bi;
        if (bb_board_get_info(&bi) == BB_OK) {
            s->ota_validated = bi.ota_validated;
        }
    }

#if BB_PUB_INFO_EMIT_OTA_READY
    s->ota_ready = bb_ota_pull_heap_ready();
#endif

    // time_valid reflects current NTP sync state (dynamic).
    s->time_valid = false;
    if (bb_ntp_is_synced()) {
        time_t now = time(NULL);
        s->time_valid = (now >= (time_t)1704067200LL);
    }

    {
        bb_mem_stats_t ms;
        bb_mem_get_stats(&ms);
        s->bb_mem_out  = ms.outstanding_bytes;
        s->bb_mem_peak = ms.peak_outstanding;
        s->bb_mem_fail = ms.alloc_fail;
    }

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
    bb_json_obj_set_number(obj, "wdt_resets",    (double)s->wdt_resets);
    bb_json_obj_set_bool  (obj, "ota_validated", s->ota_validated);
#if BB_PUB_INFO_EMIT_OTA_READY
    bb_json_obj_set_bool  (obj, "ota_ready",     s->ota_ready);
#endif
    bb_json_obj_set_bool  (obj, "time_valid",    s->time_valid);
    bb_json_obj_set_number(obj, "bb_mem_out",    (double)s->bb_mem_out);
    bb_json_obj_set_number(obj, "bb_mem_peak",   (double)s->bb_mem_peak);
    bb_json_obj_set_number(obj, "bb_mem_fail",   (double)s->bb_mem_fail);
}

// ---------------------------------------------------------------------------
// Schema + Registration
// ---------------------------------------------------------------------------

// InfoTelemetry — sink-only topic, dynamic runtime metrics only.
// Static device-identity fields (version, board, chip_model, mac, etc.)
// live in the MetaTelemetry topic (TA-505 PR-2).
static const char k_info_telemetry_schema[] =
    "{\"title\":\"InfoTelemetry\",\"type\":\"object\","
    "\"properties\":{"
    "\"heap_internal_free\":{\"type\":\"number\"},"
    "\"heap_internal_total\":{\"type\":\"number\"},"
    "\"heap_internal_largest_block\":{\"type\":\"number\"},"
    "\"heap_internal_min_free\":{\"type\":\"number\"},"
    "\"psram_free\":{\"type\":\"number\"},"
    "\"psram_total\":{\"type\":\"number\"},"
    "\"wdt_resets\":{\"type\":\"number\"},"
    "\"ota_validated\":{\"type\":\"boolean\"},"
    "\"time_valid\":{\"type\":\"boolean\"}},"
    "\"required\":[\"heap_internal_free\",\"heap_internal_total\","
    "\"heap_internal_largest_block\",\"heap_internal_min_free\","
    "\"wdt_resets\",\"ota_validated\",\"time_valid\"]}";

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
// PRE_HTTP init (after bb_pub's own PRE_HTTP registration)
// ---------------------------------------------------------------------------

bb_err_t bb_pub_info_init(void)
{
    return bb_pub_info_register();
}
