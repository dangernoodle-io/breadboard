// bb_pub_telemetry host twin — section get + patch logic + test hooks.
// Compiled on both host (test) and ESP-IDF (shared logic).
//
// TA-505 (PR-2): meta snapshot extended with static device-identity fields
// moved from the info topic.  SNAP_MAX raised to 512 (matches Kconfig default).
#include "bb_pub_telemetry.h"
#include "bb_pub.h"
#include "bb_pub_defaults.h"
#include "../bb_pub/bb_pub_priv.h"
#include "bb_board.h"
#include "bb_clock.h"
#include "bb_json.h"
#include "bb_ntp.h"
#include "bb_system.h"
#include "bb_telemetry.h"
#include "bb_log.h"
#include "bb_str.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

// Kconfig defaults for host builds.
#ifndef CONFIG_BB_PUB_TOPIC_PREFIX
#define CONFIG_BB_PUB_TOPIC_PREFIX "metrics"
#endif
// CONFIG_BB_PUB_TELEM_SNAP_MAX host fallback lives in bb_pub_defaults.h.
// The meta snapshot now includes device-identity strings (~170 bytes extra;
// TA-505), still well within the 512-byte default.

static const char *TAG = "bb_pub_telemetry";

// Interval bounds (must match bb_pub.c / Kconfig range).
#define BB_PUB_INTERVAL_MS_MIN   1000UL
#define BB_PUB_INTERVAL_MS_MAX   3600000UL

// Maximum sinks captured in the meta snapshot.  Capped at 4; with the
// identity fields added by TA-505, meta_snap_t is ~320 B on device and
// ~336 B on host — both well within CONFIG_BB_PUB_TELEM_SNAP_MAX=512.
#define BB_PUB_META_MAX_SINKS 4

// ---------------------------------------------------------------------------
// /meta telem snapshot — publisher provenance + static device identity
// ---------------------------------------------------------------------------

typedef struct {
    // Publisher provenance
    char  topic_prefix[48];
    int   sink_count;
    struct {
        char  transport[28];   /* "" = no transport label */
        bool  tls;
    } sinks[BB_PUB_META_MAX_SINKS];

    // Static device identity (moved from info topic, TA-505 PR-2).
    // Numeric fields placed before char arrays to minimise struct padding.
    int64_t  boot_epoch_s;       /* 0 when time not synced */
    size_t   flash_size;
    size_t   app_size;
    size_t   dram_static_bytes;
    size_t   rtc_used;
    size_t   rtc_total;
    char     version[32];
    char     board[32];
    char     chip_model[16];
    char     mac[18];
    char     reset_reason[16];
    char     time_source[8];     /* "sntp" or "none" */
} meta_snap_t;

// Compile-time guard: meta_snap_t must fit in the scratch buffer.
// A build failure here means the struct grew past the limit — reduce field sizes.
typedef char _meta_snap_size_check[
    sizeof(meta_snap_t) <= CONFIG_BB_PUB_TELEM_SNAP_MAX ? 1 : -1];

// meta_gather is called by bb_pub_tick_once from inside s_tick_lock (telem Phase 1).
// It MUST NOT call bb_pub_get_status / bb_pub_sink_info which re-acquire that
// non-recursive mutex → self-deadlock.  Use the nolock variants instead.
// Board/system/ntp accessors do not touch bb_pub's lock and are safe here.
static bool meta_gather(void *snap_buf, void *ctx)
{
    (void)ctx;
    meta_snap_t *m = (meta_snap_t *)snap_buf;
    memset(m, 0, sizeof(*m));

    // --- Publisher provenance ---
    bb_strlcpy(m->topic_prefix, CONFIG_BB_PUB_TOPIC_PREFIX, sizeof(m->topic_prefix));

    bb_pub_status_t st;
    bb_pub_get_status_nolock(&st);   /* caller (tick) holds s_tick_lock */
    m->sink_count = st.sink_count;

    int cap = st.sink_count < BB_PUB_META_MAX_SINKS ? st.sink_count : BB_PUB_META_MAX_SINKS;
    for (int i = 0; i < cap; i++) {
        const char *transport = NULL;
        bool tls = false;
        bb_pub_sink_info_nolock(i, &transport, &tls);   /* caller holds s_tick_lock */
        m->sinks[i].transport[0] = '\0';
        if (transport) {
            bb_strlcpy(m->sinks[i].transport, transport,
                       sizeof(m->sinks[i].transport));
        }
        m->sinks[i].tls = tls;
    }

    // --- Static device identity (moved from info topic, TA-505 PR-2) ---
    {
        const char *ver = bb_system_get_version();
        if (ver) {
            bb_strlcpy(m->version, ver, sizeof(m->version));
        }
    }
    {
        bb_board_info_t bi;
        if (bb_board_get_info(&bi) == BB_OK) {
            bb_strlcpy(m->board,      bi.board,      sizeof(m->board));
            bb_strlcpy(m->chip_model, bi.chip_model, sizeof(m->chip_model));
        }
    }
    bb_board_get_mac(m->mac, sizeof(m->mac));
    bb_board_get_reset_reason(m->reset_reason, sizeof(m->reset_reason));

    m->flash_size        = (size_t)bb_board_get_flash_size();
    m->app_size          = (size_t)bb_board_get_app_size();
    m->dram_static_bytes = bb_board_dram_static_bytes();
    m->rtc_used          = bb_board_rtc_used();
    m->rtc_total         = bb_board_rtc_total();

    // boot_epoch_s and time_source: static once synced (boot epoch never changes).
    bb_strlcpy(m->time_source, "none", sizeof(m->time_source));
    if (bb_ntp_is_synced()) {
        time_t now = time(NULL);
        if (now >= (time_t)1704067200LL) {
            int64_t uptime_s = (int64_t)bb_clock_now_ms() / 1000;
            m->boot_epoch_s = (int64_t)now - uptime_s;
            bb_strlcpy(m->time_source, "sntp", sizeof(m->time_source));
        }
    }

    return true;   /* always publish provenance, even with zero sinks */
}

static void meta_serialize(bb_json_t obj, const void *snap)
{
    const meta_snap_t *m = (const meta_snap_t *)snap;

    // Publisher provenance
    bb_json_obj_set_string(obj, "topic_prefix", m->topic_prefix);
    bb_json_obj_set_int   (obj, "sink_count",   (int64_t)m->sink_count);

    bb_json_t arr = bb_json_arr_new();
    if (!arr) return;

    int cap = m->sink_count < BB_PUB_META_MAX_SINKS ? m->sink_count : BB_PUB_META_MAX_SINKS;
    for (int i = 0; i < cap; i++) {
        bb_json_t sink_obj = bb_json_obj_new();
        if (!sink_obj) continue;
        if (m->sinks[i].transport[0] != '\0') {
            bb_json_obj_set_string(sink_obj, "transport", m->sinks[i].transport);
            bb_json_obj_set_bool  (sink_obj, "tls",       m->sinks[i].tls);
        } else {
            bb_json_obj_set_null(sink_obj, "transport");
        }
        bb_json_arr_append_obj(arr, sink_obj);
        /* sink_obj ownership transferred to arr — do NOT free here */
    }

    bb_json_obj_set_arr(obj, "sinks", arr);
    /* arr ownership transferred to obj — do NOT free here */

    // Static device identity (moved from info topic, TA-505 PR-2)
    bb_json_obj_set_string(obj, "version",          m->version);
    bb_json_obj_set_string(obj, "board",            m->board);
    bb_json_obj_set_string(obj, "chip_model",       m->chip_model);
    bb_json_obj_set_string(obj, "mac",              m->mac);
    bb_json_obj_set_string(obj, "reset_reason",     m->reset_reason);
    bb_json_obj_set_string(obj, "time_source",      m->time_source);
    bb_json_obj_set_number(obj, "flash_size",       (double)m->flash_size);
    bb_json_obj_set_number(obj, "app_size",         (double)m->app_size);
    bb_json_obj_set_number(obj, "dram_static_bytes",(double)m->dram_static_bytes);
    bb_json_obj_set_number(obj, "rtc_used",         (double)m->rtc_used);
    bb_json_obj_set_number(obj, "rtc_total",        (double)m->rtc_total);
    bb_json_obj_set_number(obj, "boot_epoch_s",     (double)m->boot_epoch_s);
}

// ---------------------------------------------------------------------------
// /api/telemetry publisher section — GET
// ---------------------------------------------------------------------------

static void pub_sinks_add(bb_json_t section)
{
    bb_pub_status_t st;
    bb_pub_get_status(&st);

    bb_json_t arr = bb_json_arr_new();
    if (!arr) return;

    int cap = st.sink_count < BB_PUB_META_MAX_SINKS ? st.sink_count : BB_PUB_META_MAX_SINKS;
    for (int i = 0; i < cap; i++) {
        const char *transport = NULL;
        bool tls = false;
        bb_pub_sink_info(i, &transport, &tls);
        bb_json_t sink_obj = bb_json_obj_new();
        if (!sink_obj) continue;
        if (transport) {
            bb_json_obj_set_string(sink_obj, "transport", transport);
            bb_json_obj_set_bool  (sink_obj, "tls",       tls);
        } else {
            bb_json_obj_set_null(sink_obj, "transport");
        }
        bb_json_arr_append_obj(arr, sink_obj);
        /* sink_obj ownership transferred to arr — do NOT free here */
    }

    bb_json_obj_set_arr(section, "sinks", arr);
    /* arr ownership transferred to section — do NOT free here */
}

static void pub_section_get(bb_json_t section, void *ctx)
{
    (void)ctx;

    bb_pub_status_t st;
    bb_pub_get_status(&st);

    uint32_t now_ms = bb_clock_now_ms();
    int32_t  age_ms = -1;
    if (st.published_ever) {
        age_ms = (int32_t)(now_ms - st.last_publish_ms);
    }

    bb_json_obj_set_number(section, "interval_ms",         (double)bb_pub_get_interval_ms());
    bb_json_obj_set_bool  (section, "enabled",             bb_pub_is_enabled());
    // available: true when the periodic worker was successfully started.
    // False on AUTOREGISTER=n builds or when bb_pub_start failed — a reboot
    // will NOT enable the publisher on such builds (B1-398).
    bb_json_obj_set_bool  (section, "available",           st.available);
    bb_json_obj_set_string(section, "topic_prefix",        CONFIG_BB_PUB_TOPIC_PREFIX);
    bb_json_obj_set_number(section, "source_count",        (double)st.source_count);
    bb_json_obj_set_number(section, "sink_count",          (double)st.sink_count);
    bb_json_obj_set_bool  (section, "last_publish_ok",     st.last_publish_ok);
    bb_json_obj_set_number(section, "last_publish_age_ms", (double)age_ms);
    bb_json_obj_set_bool  (section, "published_ever",      st.published_ever);

    bb_pub_buffer_stats_t buf;
    bb_pub_buffer_stats(&buf);
    bb_json_obj_set_number(section, "buffer_count",   (double)buf.count);
    bb_json_obj_set_number(section, "buffer_dropped", (double)buf.dropped);

    // Provenance: sinks[] array (B1-388 — transport/tls removed from payloads).
    pub_sinks_add(section);
}

// ---------------------------------------------------------------------------
// /api/telemetry publisher section — PATCH
// ---------------------------------------------------------------------------

static bb_err_t pub_section_patch(bb_json_t section_patch, void *ctx)
{
    (void)ctx;

    // interval_ms — optional field; validate and apply if present.
    double interval_val = 0.0;
    if (bb_json_obj_get_number(section_patch, "interval_ms", &interval_val)) {
        if (interval_val < (double)BB_PUB_INTERVAL_MS_MIN ||
            interval_val > (double)BB_PUB_INTERVAL_MS_MAX) {
            bb_log_w(TAG, "patch: interval_ms %.0f out of range [%lu, %lu]",
                     interval_val,
                     (unsigned long)BB_PUB_INTERVAL_MS_MIN,
                     (unsigned long)BB_PUB_INTERVAL_MS_MAX);
            return BB_ERR_INVALID_ARG;
        }
        bb_err_t err = bb_pub_set_interval_ms((uint32_t)interval_val);
        if (err != BB_OK) return err;
    }

    // enabled — optional field; apply if present.
    bool enabled_val = false;
    if (bb_json_obj_get_bool(section_patch, "enabled", &enabled_val)) {
        bb_err_t err = bb_pub_set_enabled(enabled_val);
        if (err != BB_OK) return err;
        // Note (B1-398): if enabled_val=true but the publisher is not available
        // (AUTOREGISTER=n build), we still persist the value (harmless; takes
        // effect on a future build with AUTOREGISTER=y). The route handler
        // checks bb_pub_get_status().available after dispatch and returns
        // reboot_required=false + publisher_unavailable=true in that case.
    }

    return BB_OK;
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------

bb_err_t bb_pub_telemetry_init(void)
{
    // Register /api/telemetry "publisher" section.
    static const char k_pub_schema_props[] =
        "{\"type\":\"object\","
        "\"properties\":{"
        "\"interval_ms\":{\"type\":\"number\"},"
        "\"enabled\":{\"type\":\"boolean\"},"
        "\"available\":{\"type\":\"boolean\","
            "\"description\":\"true when the publisher worker is running; "
            "false on AUTOREGISTER=n builds where a reboot will not start it\"},"
        "\"topic_prefix\":{\"type\":\"string\"},"
        "\"source_count\":{\"type\":\"number\"},"
        "\"sink_count\":{\"type\":\"number\"},"
        "\"last_publish_ok\":{\"type\":\"boolean\"},"
        "\"last_publish_age_ms\":{\"type\":\"number\"},"
        "\"published_ever\":{\"type\":\"boolean\"},"
        "\"buffer_count\":{\"type\":\"number\"},"
        "\"buffer_dropped\":{\"type\":\"number\"},"
        "\"sinks\":{\"type\":\"array\",\"items\":{\"type\":\"object\"}}}}";
    bb_err_t err = bb_telemetry_register_section_ex("publisher", pub_section_get,
                                                     pub_section_patch, NULL,
                                                     k_pub_schema_props);
    if (err != BB_OK) return err;

    // Register "meta" MQTT topic: serializes once per tick via bb_pub_register_telemetry.
    // BB_PUB_TELEM_SINKS only (no SSE — meta is a sink-delivery topic, not an event stream).
    static const bb_pub_telemetry_cfg_t k_meta_cfg = {
        .topic     = "meta",
        .gather    = meta_gather,
        .serialize = meta_serialize,
        .snap_size = sizeof(meta_snap_t),
        .flags     = BB_PUB_TELEM_SINKS,
        .ctx       = NULL,
        .retain    = true,
        .cadence   = BB_PUB_CADENCE_ON_CHANGE,
    };
    err = bb_pub_register_telemetry(&k_meta_cfg);
    if (err != BB_OK) {
        // Not fatal — meta is provenance; log but continue.
        bb_log_w(TAG, "meta telem source registration failed: %d", err);
    }

    return BB_OK;
}

// ---------------------------------------------------------------------------
// Test hooks
// ---------------------------------------------------------------------------

#ifdef BB_PUB_TELEMETRY_TESTING

void bb_pub_telemetry_reset_for_test(void)
{
    bb_pub_test_reset();
}

void bb_pub_telemetry_section_get_for_test(bb_json_t section, void *ctx)
{
    pub_section_get(section, ctx);
}

bb_err_t bb_pub_telemetry_section_patch_for_test(bb_json_t patch, void *ctx)
{
    return pub_section_patch(patch, ctx);
}

// Expose meta_gather and meta_serialize so tests can register a telem source
// that exercises the nolock code path via bb_pub_tick_once (deadlock check).
bool bb_pub_telemetry_meta_gather_for_test(void *snap_buf, void *ctx)
{
    return meta_gather(snap_buf, ctx);
}

void bb_pub_telemetry_meta_serialize_for_test(bb_json_t obj, const void *snap)
{
    meta_serialize(obj, snap);
}

size_t bb_pub_telemetry_meta_snap_size_for_test(void)
{
    return sizeof(meta_snap_t);
}

#endif /* BB_PUB_TELEMETRY_TESTING */
