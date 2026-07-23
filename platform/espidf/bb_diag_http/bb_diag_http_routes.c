// bb_diag_http_routes — relocated verbatim from components/bb_diag/
// bb_diag_routes.c (B1-1153, KB 1477): bb_diag itself is bb_http_server-free
// after this split, so this legacy exact-route surface (GET/DELETE
// /api/diag/boot, GET/DELETE /api/diag/panic, GET /api/diag/coredump, GET
// /api/diag/heap-check, GET /api/diag/tasks, GET /api/diag/sockets) now
// lives in its own component, same Kconfig gate (CONFIG_BB_DIAG_ROUTES). The
// B1-1153 move itself was a pure relocation (no behavior change); since
// then this file has also absorbed the /api/diag/heap retirement --
// GET /api/diag/heap[?check=true] is gone, its integrity check reshaped into
// the standalone GET /api/diag/heap-check route below.
#include "bb_diag.h"
#include "bb_diag_http.h"
#include "bb_cache.h"
#include "bb_diag_boot_wire.h"
#include "bb_diag_event_priv.h"
#include "../../../components/bb_diag_http/bb_diag_heap_check_wire_priv.h"
#include "../../../components/bb_diag_http/bb_diag_panic_get_wire_priv.h"
#include "../../../components/bb_diag_http/bb_diag_sockets_get_wire_priv.h"
#include "../../../components/bb_diag_http/bb_diag_tasks_get_wire_priv.h"
#include "bb_data.h"
#include "bb_http.h"
#include "bb_http_server.h"
#include "bb_http_serialize_stream.h"
#include "bb_log.h"
#include "bb_openapi.h"
#include "bb_config.h"
#include "bb_nv_namespaces.h"
#include "bb_nv_keys.h"
#include "bb_ntp.h"
#include "bb_ota_validator.h"
#include "bb_system.h"
#include "bb_reboot_reason.h"
#include "bb_mem.h"
#include "bb_task_registry.h"
#include "bb_task.h"
#include "bb_clock.h"
#include "bb_str.h"

#include "esp_system.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdlib.h>
#include <string.h>
#ifdef CONFIG_BB_DIAG_PANIC_COREDUMP
#include "esp_core_dump.h"
#include "esp_partition.h"
#include <stdio.h>
#endif
#include <time.h>

#include "lwip/tcpip.h"
#include "lwip/tcp.h"
#include "lwip/ip_addr.h"
#include "lwip/priv/tcp_priv.h"

static const char *TAG = "bb_diag_http_routes";

// Reboot-reason SSOT (B1-527 PR-A) — latched once at boot by load_reboot_record().
static bb_reset_source_t s_reboot_src        = BB_RESET_SRC_UNKNOWN;
static char              s_reboot_detail[49] = {0};
static uint32_t          s_reboot_epoch_s    = 0;
static uint32_t          s_reboot_uptime_s   = 0;

// Rolling reboot history ring (B1-527 PR-B) — latched once at boot alongside
// the fields above, by load_reboot_record(). NOT cleared-on-read.
static bb_reboot_history_t s_reboot_history = {0};

// Reboot-reason SSOT persisted fields, round-tripped through bb_config
// (typed layer over bb_storage) rather than bb_nv's generic KV forwarder
// (B1-756, bb_nv dissolution epic B1-708) — bb_config's STR encoding
// resolves to the SAME nvs_get_str/nvs_set_str calls bb_nv_get_str/set_str/
// erase made (both are thin forwarders to bb_storage_nvs, see
// bb_storage_nvs.h), so the namespace/key/STR-typed on-flash format below is
// byte-compatible with what this component previously read/wrote via bb_nv.
static const bb_config_field_t s_reboot_last_field = {
    .id          = "diag.reboot.last",
    .type        = BB_CONFIG_STR,
    .addr        = { .backend = "nvs", .ns_or_dir = BB_REBOOT_NVS_NS, .key = BB_REBOOT_KEY_LAST },
    .max_len     = BB_REBOOT_RECORD_STR_MAX,
    .def         = { .str = "" },
    .has_default = true,
};

static const bb_config_field_t s_reboot_history_field = {
    .id          = "diag.reboot.history",
    .type        = BB_CONFIG_STR,
    .addr        = { .backend = "nvs", .ns_or_dir = BB_REBOOT_NVS_NS, .key = BB_REBOOT_KEY_HISTORY },
    .max_len     = BB_REBOOT_HISTORY_STR_MAX,
    .def         = { .str = "" },
    .has_default = true,
};

// Read+decode+erase the clear-on-read reboot record. Only trusted when this
// boot's reset reason is software (BB_RESET_REASON_SW) — a record left over
// from a stale/aborted write (e.g. a subsequent panic before the intended
// reboot completed) must not be misattributed to a different reset class.
static void load_reboot_record(void)
{
    char buf[BB_REBOOT_RECORD_STR_MAX] = {0};
    size_t out_len = 0;
    bb_err_t rc = bb_config_get_str(&s_reboot_last_field, buf, sizeof(buf), &out_len);
    // bb_config_get_str's blob-fallback path does not itself NUL-terminate
    // (B1-756 trap 1) -- a truncating read (stored value >= sizeof(buf))
    // fills the ENTIRE buffer and still returns BB_OK, so the zero-init
    // above is not a safety net for that case. Terminate explicitly based
    // on the reported length before treating buf as a C string, clamping
    // to the last byte on a genuine truncation (out_len >= cap).
    if (rc == BB_OK) {
        size_t term = (out_len < sizeof(buf)) ? out_len : sizeof(buf) - 1;
        buf[term] = '\0';
    }

    // Only decode when the NVS read actually succeeded — on a non-NOT_FOUND
    // error (corruption/foreign entry), bb_config_get_str may leave buf
    // untouched, and buf is zero-initialized above so decode would just
    // fail cleanly on "" rather than reading anything meaningful anyway.
    bb_reboot_record_t rec;
    bool have_rec = (rc == BB_OK) && bb_reboot_record_decode(buf, &rec);

    // Clear-on-read regardless of decode outcome (including the NVS-error
    // path above) — a malformed/stale record must not linger and get
    // misread on a future boot.
    bb_config_erase(&s_reboot_last_field);

    bool sw_reset = (bb_system_get_reset_reason() == BB_RESET_REASON_SW);
    if (have_rec && sw_reset) {
        s_reboot_src = (bb_reset_source_t)rec.src;
        bb_strlcpy(s_reboot_detail, rec.detail, sizeof(s_reboot_detail));
        s_reboot_epoch_s  = rec.epoch_s;
        s_reboot_uptime_s = rec.uptime_s;
    } else {
        s_reboot_src         = BB_RESET_SRC_UNKNOWN;
        s_reboot_detail[0]   = '\0';
        s_reboot_epoch_s     = 0;
        s_reboot_uptime_s    = 0;
    }

    // Append this boot to the rolling history ring (B1-527 PR-B). Unlike the
    // record above, the ring is NOT cleared-on-read — it accumulates across
    // boots, including untagged/hardware resets (pushed as src=unknown,
    // epoch_s=0, uptime_s=0 — mirrors the effective reboot_reason computed
    // above exactly).
    {
        char hist_buf[BB_REBOOT_HISTORY_STR_MAX] = {0};
        size_t hist_out_len = 0;
        bb_err_t hrc = bb_config_get_str(&s_reboot_history_field, hist_buf, sizeof(hist_buf), &hist_out_len);
        // Same rationale as buf above -- terminate explicitly based on the
        // reported length before treating hist_buf as a C string (B1-756
        // trap 1).
        if (hrc == BB_OK) {
            size_t hist_term = (hist_out_len < sizeof(hist_buf)) ? hist_out_len : sizeof(hist_buf) - 1;
            hist_buf[hist_term] = '\0';
        }
        bb_reboot_history_t hist;
        memset(&hist, 0, sizeof(hist));
        if (hrc == BB_OK) {
            bb_reboot_history_decode(hist_buf, &hist);
            // On decode failure, hist stays zero-initialized (fresh ring) —
            // the safe fallback for corrupted/foreign NVS data.
        }

        bb_reboot_hist_entry_t entry = {
            .src      = (uint8_t)s_reboot_src,
            .epoch_s  = s_reboot_epoch_s,
            .uptime_s = s_reboot_uptime_s,
        };
        bb_reboot_history_push(&hist, &entry);
        s_reboot_history = hist;

        if (bb_reboot_history_encode(&hist, hist_buf, sizeof(hist_buf))) {
            bb_err_t werr = bb_config_set_str(&s_reboot_history_field, hist_buf);
            if (werr != BB_OK) {
                bb_log_w(TAG, "failed to persist reboot history: %d", (int)werr);
            }
        }
    }
}

// Build a fresh diag.boot snapshot. Called both at publish time (init +
// on_validated) and at GET-request time so "now" (used for reboot_reason.age_s)
// always reflects the current wall clock, not a stale publish-time value.
static void build_boot_snap(bb_diag_boot_snap_t *snap)
{
    const char *rr = bb_system_reset_reason_str(bb_system_get_reset_reason());
    size_t rr_len = strlen(rr);
    if (rr_len >= sizeof(snap->reset_reason)) rr_len = sizeof(snap->reset_reason) - 1;
    memcpy(snap->reset_reason, rr, rr_len);
    snap->reset_reason[rr_len] = '\0';
    snap->wdt_resets        = bb_diag_abnormal_reset_count();
    snap->panic_available   = bb_diag_panic_available();
    snap->panic_boots_since = snap->panic_available ? bb_diag_panic_boots_since() : 0;
    snap->pending_verify    = !bb_ota_is_validated();
    snap->rolled_back       = bb_ota_rolled_back();

    snap->reboot_src = (uint8_t)s_reboot_src;
    bb_strlcpy(snap->reboot_detail, s_reboot_detail, sizeof(snap->reboot_detail));
    snap->reboot_epoch_s  = s_reboot_epoch_s;
    snap->reboot_uptime_s = s_reboot_uptime_s;

    snap->reboot_history = s_reboot_history;

    snap->now_epoch_valid = false;
    snap->now_epoch_s     = 0;
    if (bb_ntp_is_synced()) {
        time_t now = time(NULL);
        if (now >= (time_t)1704067200LL) {
            snap->now_epoch_valid = true;
            snap->now_epoch_s     = (uint32_t)now;
        }
    }
}

static void diag_boot_publish(void)
{
    bb_diag_boot_snap_t snap;
    build_boot_snap(&snap);
    bb_cache_update(&(bb_cache_update_t){ .key = BB_DIAG_BOOT_TOPIC, .snap = &snap });
    bb_data_touch(BB_DIAG_BOOT_TOPIC);
}

static bb_err_t panic_get_handler(bb_http_request_t *req)
{
    bool available      = bb_diag_panic_available();
    bool coredump_avail = bb_diag_panic_coredump_available();

    const char *reason_str = "unknown";
    char panic_buf[512];
    bool log_tail_ok = false;

    if (available) {
        esp_reset_reason_t reason = esp_reset_reason();
        switch (reason) {
            case ESP_RST_PANIC:    reason_str = "panic";    break;
            case ESP_RST_TASK_WDT: reason_str = "task_wdt"; break;
            case ESP_RST_INT_WDT:  reason_str = "int_wdt";  break;
            case ESP_RST_WDT:      reason_str = "wdt";      break;
            case ESP_RST_BROWNOUT: reason_str = "brownout"; break;
            default: break;
        }

        size_t panic_len = sizeof(panic_buf) - 1;
        log_tail_ok = (bb_diag_panic_get(panic_buf, &panic_len) == BB_OK);
    }

    // bb_diag_panic_coredump_get() (like bb_diag_panic_coredump_available()
    // above) is unconditionally link-safe -- it returns BB_ERR_NOT_FOUND on
    // every build variant lacking CONFIG_BB_DIAG_PANIC_COREDUMP (real
    // ESP-IDF without the Kconfig, safe-stub, host) -- so no
    // `#ifdef CONFIG_BB_DIAG_PANIC_COREDUMP` gate is needed here; the
    // runtime `coredump_avail` check alone reproduces the pre-migration
    // handler's #ifdef-gated block byte-for-byte (locked design decision,
    // see bb_diag_panic_get_wire_priv.h's banner).
    bb_diag_panic_summary_t summary;
    memset(&summary, 0, sizeof(summary));
    bool have_summary = coredump_avail && (bb_diag_panic_coredump_get(&summary) == BB_OK);

    bb_diag_panic_get_wire_t snap;
    bb_diag_panic_get_wire_fill(&snap, available, coredump_avail,
                                bb_diag_panic_boots_since(), reason_str,
                                log_tail_ok, panic_buf,
                                have_summary ? &summary : NULL);

    return bb_http_serialize_stream(req, &bb_diag_panic_get_wire_desc, &snap);
}

// GET /api/diag/boot — compact boot-anomaly summary, rendered via bb_data
// against the "diag.boot" binding (B1-1053 PR1: cut over off the retired
// bb_json bb_cache serializer). Response shape changed: this used to emit a
// BARE object; it now emits the {"ts_ms":N,"data":{...}} envelope (see
// bb_diag_boot_render_envelope()'s doc for the ts_ms semantic).
static bb_err_t boot_get_handler(bb_http_request_t *req)
{
    // DO NOT DELETE this refresh as "redundant with bb_data_render()'s
    // per-render gather" -- it is NOT redundant for this key. VERIFIED
    // against bb_diag_boot_gather()'s actual body (bb_diag_boot_wire.c)
    // before this cutover: that gather hook is a PURE PASS-THROUGH -- it
    // only widens whatever bb_cache_get_raw() currently holds, it does not
    // itself recompute now_epoch_s/age_s. If this bb_cache_update() refresh
    // were removed, reboot_reason.age_s would freeze at whatever it was at
    // the last publish (init/on_validated) instead of reflecting this GET.
    // PR2 (the other 5 dissolved-bb_event producers): re-verify this same
    // question per producer -- do NOT assume "gather refreshes" generalizes,
    // it does not hold for diag.boot and may not hold for the others either.
    bb_diag_boot_snap_t fresh;
    build_boot_snap(&fresh);
    bb_cache_update(&(bb_cache_update_t){ .key = BB_DIAG_BOOT_TOPIC, .snap = &fresh });

    return bb_diag_boot_render_envelope(req);
}

// DELETE /api/diag/boot — clear panic log + abnormal-reset counter
static bb_err_t boot_delete_handler(bb_http_request_t *req)
{
    bb_diag_panic_clear();
    bb_diag_abnormal_reset_count_clear();
    bb_http_resp_set_status(req, 204);
    return bb_http_resp_send_chunk(req, NULL, 0);
}

static const bb_route_response_t s_boot_get_responses[] = {
    { 200, "application/json",
      "{\"type\":\"object\","
      "\"properties\":{"
      "\"ts_ms\":{\"type\":\"integer\"},"
      "\"data\":{\"type\":\"object\","
      "\"properties\":{"
      "\"reset_reason\":{\"type\":\"string\"},"
      "\"wdt_resets\":{\"type\":\"integer\"},"
      "\"panic\":{\"type\":\"object\","
      "\"properties\":{"
      "\"available\":{\"type\":\"boolean\"},"
      "\"boots_since\":{\"type\":\"integer\"}},"
      "\"required\":[\"available\"]},"
      "\"pending_verify\":{\"type\":\"boolean\"},"
      "\"rolled_back\":{\"type\":\"boolean\"},"
      "\"reboot_reason\":{\"type\":\"object\","
      "\"properties\":{"
      "\"source\":{\"type\":\"string\"},"
      "\"detail\":{\"type\":\"string\"},"
      "\"uptime_s\":{\"type\":\"integer\"},"
      "\"epoch_s\":{\"type\":\"integer\"},"
      "\"age_s\":{\"type\":\"integer\"}},"
      "\"required\":[\"source\",\"uptime_s\",\"epoch_s\"]},"
      "\"reboot_history\":{\"type\":\"array\",\"items\":{\"type\":\"object\","
      "\"properties\":{"
      "\"source\":{\"type\":\"string\"},"
      "\"epoch_s\":{\"type\":\"integer\"},"
      "\"uptime_s\":{\"type\":\"integer\"}},"
      "\"required\":[\"source\",\"epoch_s\",\"uptime_s\"]}}},"
      "\"required\":[\"reset_reason\",\"wdt_resets\",\"panic\",\"pending_verify\",\"rolled_back\","
      "\"reboot_reason\",\"reboot_history\"]}},"
      "\"required\":[\"ts_ms\",\"data\"]}",
      "BREAKING (B1-1053 PR1): response root changed from a bare object to the "
      "{ts_ms,data} envelope -- \\\"data\\\" carries exactly the fields this route "
      "used to emit at the root. ts_ms is the wall-clock time (ms) this response "
      "was rendered, NOT a sample time (bb_data has no notion of one for this key). "
      "data.reset_reason/wdt_resets/panic/pending_verify/rolled_back: current boot "
      "reset reason, WDT-reset count, panic availability, OTA state summary. "
      "data.reboot_reason is the semantic reboot_reason SSOT (may disagree with "
      "hardware reset_reason — e.g. an app-requested reboot still reports "
      "reset_reason=\\\"software\\\" at the hardware level while reboot_reason.source "
      "names the app-level cause; source=\\\"unknown\\\" when no semantic record was "
      "captured for this boot). age_s is omitted when epoch_s is 0 or the current "
      "wall clock is not NTP-synced. data.reboot_history is a rolling ring of the "
      "last 8 reboots, newest-first, including untagged/hardware resets "
      "(source=\\\"unknown\\\"); unlike reboot_reason it is NOT cleared on read." },
    { 0 },
};

static const bb_route_t s_boot_get_route = {
    .method    = BB_HTTP_GET,
    .path      = "/api/diag/boot",
    .tag       = "diag",
    .summary   = "Boot anomaly summary: reset reason, abnormal-reset count, panic availability",
    .responses = s_boot_get_responses,
    .handler   = boot_get_handler,
};

static const bb_route_response_t s_boot_delete_responses[] = {
    { 204, NULL, NULL, "panic log and abnormal-reset counter cleared" },
    { 0 },
};

static const bb_route_t s_boot_delete_route = {
    .method    = BB_HTTP_DELETE,
    .path      = "/api/diag/boot",
    .tag       = "diag",
    .summary   = "Clear panic log and abnormal-reset counter",
    .responses = s_boot_delete_responses,
    .handler   = boot_delete_handler,
};

// GET /api/diag/cache (folded in from the deleted bb_cache_routes component,
// B1-1121) is DELETED (B1-1119): it existed only to expose a bb_cache key's
// legacy bb_json .serialize output over REST, and bb_cache no longer ships
// that mechanism at all (bb_cache_get_serialized() is gone -- see
// bb_cache.h). Every producer had already migrated off .serialize before
// this route was deleted (B1-1146b, #998), so the route had degenerated to
// an unconditional 404/501 debug dump with no remaining data source -- not
// worth keeping as a stub. A key's canonical value is reachable via
// bb_data-rendered endpoints instead.

static const bb_route_response_t s_panic_get_responses[] = {
    { 200, "application/json",
      "{\"type\":\"object\","
      "\"properties\":{"
      "\"available\":{\"type\":\"boolean\"},"
      "\"boots_since\":{\"type\":\"integer\"},"
      "\"reset_reason\":{\"type\":\"string\"},"
      "\"log_tail\":{\"type\":\"string\"},"
      "\"task\":{\"type\":\"string\"},"
      "\"exc_pc\":{\"type\":\"integer\"},"
      "\"exc_cause\":{\"type\":\"integer\"},"
      "\"backtrace\":{\"type\":\"array\",\"items\":{\"type\":\"integer\"}},"
      "\"panic_reason\":{\"type\":\"string\"},"
      "\"app_sha256\":{\"type\":\"string\"}},"
      "\"required\":[\"available\"]}",
      "panic log status, log tail, coredump backtrace, and panic reason text (when available)" },
    { 0 },
};

static const bb_route_t s_panic_get_route = {
    .method   = BB_HTTP_GET,
    .path     = "/api/diag/panic",
    .tag      = "diag",
    .summary  = "Get panic log from previous abnormal boot",
    .responses = s_panic_get_responses,
    .handler  = panic_get_handler,
};

#ifdef CONFIG_BB_DIAG_PANIC_TRIGGER
static bb_err_t panic_trigger_handler(bb_http_request_t *req)
{
    (void)req;
    volatile int *p = NULL;
    *p = 0;
    return BB_OK;
}

static const bb_route_response_t s_panic_trigger_responses[] = {
    { 500, NULL, NULL, "never returned — handler panics before sending response" },
    { 0 },
};

static const bb_route_t s_panic_trigger_route = {
    .method    = BB_HTTP_POST,
    .path      = "/api/diag/panic/trigger",
    .tag       = "diag",
    .summary   = "Force a panic via null dereference (debug builds only)",
    .responses = s_panic_trigger_responses,
    .handler   = panic_trigger_handler,
};
#endif

#ifdef CONFIG_BB_DIAG_PANIC_COREDUMP
static bb_err_t coredump_get_handler(bb_http_request_t *req)
{
    size_t size = bb_diag_panic_coredump_size();
    if (size == 0) {
        bb_http_resp_set_status(req, 404);
        bb_http_json_obj_stream_t obj;
        bb_http_resp_json_obj_begin(req, &obj);
        bb_http_resp_json_obj_set_str(&obj, "error", "no coredump available");
        bb_http_resp_json_obj_end(&obj);
        return BB_ERR_NOT_FOUND;
    }

    /* Parse ?consume query param: truthy = "1" or "true". */
    char consume_val[8] = {0};
    bool consume = false;
    if (bb_http_req_query_key_value(req, "consume", consume_val, sizeof(consume_val)) == BB_OK) {
        consume = (strcmp(consume_val, "1") == 0 || strcmp(consume_val, "true") == 0);
    }

    // Allocate in chunks to keep heap pressure manageable on a panicked device.
    // 4KB chunks; one buffer for the whole file would be ~64KB which we'd rather avoid.
    enum { CHUNK = 4096 };
    uint8_t *chunk = bb_malloc_prefer_spiram(CHUNK);
    if (!chunk) {
        bb_http_resp_set_status(req, 500);
        bb_http_json_obj_stream_t obj;
        bb_http_resp_json_obj_begin(req, &obj);
        bb_http_resp_json_obj_set_str(&obj, "error", "alloc failed");
        bb_http_resp_json_obj_end(&obj);
        return BB_ERR_NO_SPACE;
    }

    bb_http_resp_set_status(req, 200);
    bb_http_resp_set_type(req, "application/octet-stream");
    char content_len[24];
    snprintf(content_len, sizeof content_len, "%zu", size);
    bb_http_resp_set_header(req, "Content-Length", content_len);
    bb_http_resp_set_header(req, "Content-Disposition", "attachment; filename=\"coredump.bin\"");

    /* Emit X-Coredump-App-SHA256 so the caller knows which build the dump is from
     * without having to download and decode it. */
    char app_sha[BB_DIAG_PANIC_APP_SHA256_MAX];
    if (bb_diag_panic_app_sha(app_sha, sizeof(app_sha)) == BB_OK) {
        bb_http_resp_set_header(req, "X-Coredump-App-SHA256", app_sha);
    }

    // Use the partition API directly for chunked reads — no need to allocate the
    // entire coredump buffer to satisfy the "fits in max_len" precondition of
    // bb_diag_panic_coredump_read_bytes.
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, NULL);
    if (!part) {
        bb_mem_free(chunk);
        bb_http_resp_set_status(req, 500);
        bb_http_json_obj_stream_t obj;
        bb_http_resp_json_obj_begin(req, &obj);
        bb_http_resp_json_obj_set_str(&obj, "error", "partition not found");
        bb_http_resp_json_obj_end(&obj);
        return BB_ERR_INVALID_STATE;
    }
    size_t addr = 0, total = 0;
    if (esp_core_dump_image_get(&addr, &total) != ESP_OK) {
        bb_mem_free(chunk);
        bb_http_resp_set_status(req, 500);
        bb_http_json_obj_stream_t obj;
        bb_http_resp_json_obj_begin(req, &obj);
        bb_http_resp_json_obj_set_str(&obj, "error", "image_get failed");
        bb_http_resp_json_obj_end(&obj);
        return BB_ERR_INVALID_STATE;
    }
    size_t part_offset = (addr >= part->address) ? (addr - part->address) : 0;

    size_t sent = 0;
    bb_err_t err = BB_OK;
    while (sent < total) {
        size_t want = (total - sent < CHUNK) ? (total - sent) : CHUNK;
        if (esp_partition_read(part, part_offset + sent, chunk, want) != ESP_OK) {
            err = BB_ERR_INVALID_STATE;
            break;
        }
        err = bb_http_resp_send_chunk(req, (const char *)chunk, (int)want);
        if (err != BB_OK) break;
        sent += want;
    }
    // Terminate the chunked response (NULL/0 sentinel).
    if (err == BB_OK) {
        err = bb_http_resp_send_chunk(req, NULL, 0);
    }
    bb_mem_free(chunk);

    /* Erase only after a fully successful stream — a dropped connection must not
     * silently discard the crash evidence. */
    if (err == BB_OK && consume) {
        bb_diag_panic_coredump_erase();
    }

    return err;
}

static const bb_route_param_t s_coredump_get_params[] = {
    {
        .name        = "consume",
        .in          = "query",
        .description = "Set to 1 or true to erase the coredump from flash after a fully "
                       "successful transfer. A dropped download does NOT erase the dump.",
        .required    = false,
        .schema_type = "string",
    },
};

static const bb_route_response_t s_coredump_get_responses[] = {
    { 200, "application/octet-stream", NULL,
      "raw coredump bytes; includes X-Coredump-App-SHA256 header with the crashing "
      "app ELF SHA256 hex string (identifies the build without downloading). "
      "Pass ?consume=1 to erase the coredump from flash only after the full image "
      "has been streamed successfully — a dropped download does NOT erase the dump." },
    { 404, "application/json",
      "{\"type\":\"object\","
      "\"properties\":{\"error\":{\"type\":\"string\"}},"
      "\"required\":[\"error\"]}",
      "no coredump available" },
    { 500, "application/json",
      "{\"type\":\"object\","
      "\"properties\":{\"error\":{\"type\":\"string\"}},"
      "\"required\":[\"error\"]}",
      "allocation failure, partition not found, or coredump image_get failed" },
    { 0 },
};

static const bb_route_t s_coredump_get_route = {
    .method            = BB_HTTP_GET,
    .path              = "/api/diag/coredump",
    .tag               = "diag",
    .summary           = "Download raw coredump partition bytes; ?consume=1 erases after successful transfer",
    .responses         = s_coredump_get_responses,
    .parameters        = s_coredump_get_params,
    .parameters_count  = 1,
    .handler           = coredump_get_handler,
};
#endif /* CONFIG_BB_DIAG_PANIC_COREDUMP */

// --- heap-check ---

// GET /api/diag/heap-check — on-demand heap integrity walk. Kept as a
// standalone route (not folded into /api/diag/meminfo, which covers the
// per-cap stats this route used to also emit): heap_caps_check_integrity_all
// is an expensive, interrupts-disabled blocking walk, semantically wrong for
// a passive/pollable stats section. Driven via the bb_serialize descriptor
// (B1-1054 diag conversion) rather than hand-streamed cJSON -- see
// bb_diag_heap_check_wire_priv.h for the {"integrity_ok":<bool>} shape;
// output is byte-identical to the pre-migration emitter.
static bb_err_t heap_check_get_handler(bb_http_request_t *req)
{
    bool integrity_ok = heap_caps_check_integrity_all(true);

    bb_diag_heap_check_wire_t snap;
    bb_diag_heap_check_wire_fill(&snap, integrity_ok);

    return bb_http_serialize_stream(req, &bb_diag_heap_check_wire_desc, &snap);
}

static const bb_route_response_t s_heap_check_get_responses[] = {
    { 200, "application/json",
      "{\"type\":\"object\","
      "\"properties\":{"
      "\"integrity_ok\":{\"type\":\"boolean\"}},"
      "\"required\":[\"integrity_ok\"]}",
      "result of an on-demand heap_caps_check_integrity_all() walk" },
    { 0 },
};

static const bb_route_t s_heap_check_get_route = {
    .method    = BB_HTTP_GET,
    .path      = "/api/diag/heap-check",
    .tag       = "diag",
    .summary   = "Run an on-demand heap integrity check (interrupts-disabled blocking walk)",
    .responses = s_heap_check_get_responses,
    .handler   = heap_check_get_handler,
};

// --- tasks ---
// Requires CONFIG_FREERTOS_USE_TRACE_FACILITY=y (provides uxTaskGetSystemState).

#if CONFIG_FREERTOS_USE_TRACE_FACILITY
// Driven via the bb_serialize descriptor (B1-1191 diag conversion) rather
// than hand-streamed cJSON -- see bb_diag_tasks_get_wire_priv.h for the
// full shape; output is byte-identical to the pre-migration emitter. The
// two ESP-IDF/Kconfig gates (CONFIG_FREERTOS_VTASKLIST_INCLUDE_COREID,
// CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS) stay HERE, in the ESP-IDF-only
// gather -- each is resolved to a plain (bool present, int64_t value) pair
// BEFORE calling bb_diag_tasks_get_wire_fill_row(), so the wire descriptor
// itself carries zero `#if CONFIG_*` (see the priv header's banner). Single
// pass, no fixed row-count cap: uxTaskGetNumberOfTasks() gives an exact `n`
// up front, so both the TaskStatus_t array and the portable row array are
// heap-allocated to that exact size and streamed via BB_ARR_STREAM.
static bb_err_t tasks_get_handler(bb_http_request_t *req)
{
    UBaseType_t n = uxTaskGetNumberOfTasks();
    TaskStatus_t *arr = bb_malloc_prefer_spiram(sizeof(TaskStatus_t) * n);
    if (!arr) {
        bb_http_resp_set_status(req, 500);
        bb_http_json_obj_stream_t err_obj;
        bb_http_resp_json_obj_begin(req, &err_obj);
        bb_http_resp_json_obj_set_str(&err_obj, "error", "alloc failed");
        bb_http_resp_json_obj_end(&err_obj);
        return BB_ERR_NO_SPACE;
    }

    uint32_t total_runtime = 0;
    UBaseType_t got = uxTaskGetSystemState(arr, n, &total_runtime);

    bb_diag_tasks_get_wire_row_t *rows =
        bb_malloc_prefer_spiram(sizeof(bb_diag_tasks_get_wire_row_t) * got);
    if (!rows) {
        bb_mem_free(arr);
        bb_http_resp_set_status(req, 500);
        bb_http_json_obj_stream_t err_obj;
        bb_http_resp_json_obj_begin(req, &err_obj);
        bb_http_resp_json_obj_set_str(&err_obj, "error", "alloc failed");
        bb_http_resp_json_obj_end(&err_obj);
        return BB_ERR_NO_SPACE;
    }

    for (UBaseType_t i = 0; i < got; i++) {
        const char *state_str = "?";
        switch (arr[i].eCurrentState) {
            case eRunning:   state_str = "running";   break;
            case eReady:     state_str = "ready";     break;
            case eBlocked:   state_str = "blocked";   break;
            case eSuspended: state_str = "suspended"; break;
            case eDeleted:   state_str = "deleted";   break;
            case eInvalid:   state_str = "invalid";   break;
            default: break;
        }

        bool    core_present = false;
        int64_t core         = 0;
#if CONFIG_FREERTOS_VTASKLIST_INCLUDE_COREID
        core_present = true;
        core         = (int64_t)arr[i].xCoreID;
#endif

        bool    runtime_present = false;
        int64_t runtime         = 0;
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
        runtime_present = true;
        runtime         = (int64_t)arr[i].ulRunTimeCounter;
#endif

        // Cross-reference bb_task_registry by name via the pure lookup fns
        // (B1-445/B1-458 PR-B) -- present iff the task self-registered.
        uint32_t reg_budget = 0;
        bool     reg_wdt    = false;
        bool     registry_present =
            bb_task_registry_lookup_budget(arr[i].pcTaskName, &reg_budget, &reg_wdt);

        uint32_t sw_timeout_ms = 0, sw_feed_age_ms = 0, sw_miss_age_ms = 0, sw_miss_count = 0;
        bool sw_wdt_present =
            bb_task_registry_lookup_sw_wdt(arr[i].pcTaskName, bb_clock_now_ms(),
                                            &sw_timeout_ms, &sw_feed_age_ms,
                                            &sw_miss_age_ms, &sw_miss_count);

        bb_diag_tasks_get_wire_fill_row(&rows[i], arr[i].pcTaskName,
                                         (int64_t)arr[i].uxCurrentPriority,
                                         (int64_t)arr[i].uxBasePriority,
                                         (int64_t)arr[i].usStackHighWaterMark,
                                         state_str,
                                         core_present, core,
                                         runtime_present, runtime,
                                         registry_present, (uint64_t)reg_budget, reg_wdt,
                                         sw_wdt_present, (uint64_t)sw_timeout_ms,
                                         (uint64_t)sw_feed_age_ms, (uint64_t)sw_miss_count,
                                         (uint64_t)sw_miss_age_ms);
    }
    bb_mem_free(arr);

    // Registry occupancy (B1-601 re-scope, was B1-471 on bb_task_registry):
    // observability for bb_task_base's own fixed-capacity pool -- the SSOT
    // table every bb_task_create() call hits -- independent of the live
    // FreeRTOS task list above (a task can be base-registered without a
    // matching TaskStatus_t entry, e.g. a name mismatch, and vice versa for
    // tasks that never went through bb_task_create()).
    bb_diag_tasks_get_wire_t snap;
    bb_diag_tasks_get_wire_fill_snap(&snap, rows, got,
                                      (uint64_t)bb_task_base_count(),
                                      (uint64_t)bb_task_base_capacity(),
                                      (uint64_t)bb_task_base_dropped());

    // `rows` must outlive this call (bb_serialize_arr_stream_from_buf()'s
    // iterator reads it lazily, not a copy) -- freed only after the stream
    // render completes.
    bb_err_t err = bb_http_serialize_stream(req, &bb_diag_tasks_get_wire_desc, &snap);
    bb_mem_free(rows);
    return err;
}

static const bb_route_response_t s_tasks_get_responses[] = {
    { 200, "application/json",
      "{\"type\":\"object\","
      "\"properties\":{"
      "\"tasks\":{\"type\":\"array\","
      "\"items\":{\"type\":\"object\","
      "\"properties\":{"
      "\"name\":{\"type\":\"string\"},"
      "\"prio\":{\"type\":\"integer\"},"
      "\"base_prio\":{\"type\":\"integer\"},"
      "\"stack_hwm\":{\"type\":\"integer\"},"
      "\"state\":{\"type\":\"string\"},"
      "\"core\":{\"type\":\"integer\"},"
      "\"runtime\":{\"type\":\"integer\"},"
      "\"stack_budget_bytes\":{\"type\":\"integer\"},"
      "\"wdt_subscribed\":{\"type\":\"boolean\"},"
      "\"sw_wdt_timeout_ms\":{\"type\":\"integer\"},"
      "\"sw_wdt_last_feed_age_ms\":{\"type\":\"integer\"},"
      "\"sw_wdt_miss_count\":{\"type\":\"integer\"},"
      "\"sw_wdt_last_miss_age_ms\":{\"type\":\"integer\"}}}},"
      "\"registry\":{\"type\":\"object\","
      "\"properties\":{"
      "\"count\":{\"type\":\"integer\"},"
      "\"capacity\":{\"type\":\"integer\"},"
      "\"dropped\":{\"type\":\"integer\"}},"
      "\"required\":[\"count\",\"capacity\",\"dropped\"]}},"
      "\"required\":[\"tasks\",\"registry\"]}",
      "task list + bb_task_base occupancy (BREAKING B1-471: response root "
      "changed from a bare array to an object with a \\\"tasks\\\" array field; "
      "B1-601 re-scope: registry.{count,capacity,dropped} moved from "
      "bb_task_registry to bb_task_base); "
      "core requires CONFIG_FREERTOS_VTASKLIST_INCLUDE_COREID; "
      "runtime requires CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS; "
      "stack_budget_bytes/wdt_subscribed present only for tasks self-registered "
      "via bb_task_registry; sw_wdt_* present only when that task's "
      "opts->sw_wdt_timeout_ms > 0; registry.{count,capacity,dropped} come from "
      "bb_task_base_{count,capacity,dropped}() -- the fixed task-creation pool "
      "every bb_task_create() call hits -- and are independent of the "
      "live FreeRTOS task list above" },
    { 500, "application/json",
      "{\"type\":\"object\","
      "\"properties\":{\"error\":{\"type\":\"string\"}},"
      "\"required\":[\"error\"]}",
      "allocation failure (malloc for TaskStatus_t array failed)" },
    { 0 },
};

static const bb_route_t s_tasks_get_route = {
    .method    = BB_HTTP_GET,
    .path      = "/api/diag/tasks",
    .tag       = "diag",
    .summary   = "List all FreeRTOS tasks with state, priority, and stack high-water mark, "
                 "plus bb_task_base occupancy "
                 "(requires CONFIG_FREERTOS_USE_TRACE_FACILITY=y; "
                 "core field: CONFIG_FREERTOS_VTASKLIST_INCLUDE_COREID; "
                 "runtime field: CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS)",
    .responses = s_tasks_get_responses,
    .handler   = tasks_get_handler,
};
#endif /* CONFIG_FREERTOS_USE_TRACE_FACILITY */

// --- sockets ---
// Walks LWIP TCP PCB lists under LOCK_TCPIP_CORE and emits a JSON snapshot
// of per-state counts and individual PCBs. Designed for hunting CLOSE_WAIT
// pile-ups that exhaust the LWIP socket pool under sustained SSE churn.
// Driven via the bb_serialize descriptor (B1-1190 diag conversion) rather
// than hand-streamed cJSON -- see bb_diag_sockets_get_wire_priv.h for the
// full shape; output is byte-identical to the pre-migration emitter. The
// row cap (BB_DIAG_SOCKETS_ROW_CAP) tracks CONFIG_LWIP_MAX_SOCKETS, so both
// the wire snapshot and this handler's own staging buffer are heap-allocated
// (see bb_diag_sockets_get_wire_priv.h's banner) -- NEVER stack locals.
static bb_err_t sockets_get_handler(bb_http_request_t *req)
{
    bb_diag_sockets_get_wire_t *dst = bb_malloc_prefer_spiram(sizeof(*dst));
    if (!dst) {
        bb_http_resp_set_status(req, 500);
        bb_http_json_obj_stream_t err_obj;
        bb_http_resp_json_obj_begin(req, &err_obj);
        bb_http_resp_json_obj_set_str(&err_obj, "error", "alloc failed");
        bb_http_resp_json_obj_end(&err_obj);
        return BB_ERR_NO_SPACE;
    }

    bb_diag_sockets_pcb_src_t *src =
        bb_malloc_prefer_spiram(sizeof(*src) * (size_t)BB_DIAG_SOCKETS_ROW_CAP);
    if (!src) {
        bb_mem_free(dst);
        bb_http_resp_set_status(req, 500);
        bb_http_json_obj_stream_t err_obj;
        bb_http_resp_json_obj_begin(req, &err_obj);
        bb_http_resp_json_obj_set_str(&err_obj, "error", "alloc failed");
        bb_http_resp_json_obj_end(&err_obj);
        return BB_ERR_NO_SPACE;
    }

    // Walk PCB lists under core lock and snapshot counts + per-entry data.
    uint32_t by_state[BB_DIAG_SOCKETS_STATE_COUNT] = {0};
    uint32_t in_use = 0;
    size_t src_count = 0;

    LOCK_TCPIP_CORE();
    struct tcp_pcb *lists[4];
    lists[0] = tcp_active_pcbs;
    lists[1] = tcp_tw_pcbs;
    lists[2] = tcp_bound_pcbs;
    lists[3] = (struct tcp_pcb *)tcp_listen_pcbs.pcbs;

    for (int li = 0; li < 4; li++) {
        for (struct tcp_pcb *p = lists[li]; p != NULL; p = p->next) {
            enum tcp_state st = p->state;
            uint32_t idx = (uint32_t)st;
            if (idx < BB_DIAG_SOCKETS_STATE_COUNT) by_state[idx]++;
            if (st != CLOSED) in_use++;
            if (src_count < (size_t)BB_DIAG_SOCKETS_ROW_CAP) {
                bb_diag_sockets_pcb_src_t *s = &src[src_count++];
                s->local_port  = p->local_port;
                s->remote_port = p->remote_port;
                s->state_idx   = idx;
                ipaddr_ntoa_r(&p->remote_ip, s->remote_ip, sizeof(s->remote_ip));
            }
        }
    }
    UNLOCK_TCPIP_CORE();

    bb_log_i(TAG, "sockets: in_use=%"PRIu32" CLOSE_WAIT=%"PRIu32" TIME_WAIT=%"PRIu32,
             in_use, by_state[CLOSE_WAIT], by_state[TIME_WAIT]);

    bb_diag_sockets_get_wire_fill(dst, (uint32_t)CONFIG_LWIP_MAX_SOCKETS, in_use,
                                  by_state, src, src_count);
    bb_mem_free(src);

    bb_err_t err = bb_http_serialize_stream(req, &bb_diag_sockets_get_wire_desc, dst);
    bb_mem_free(dst);
    return err;
}

static const bb_route_response_t s_sockets_get_responses[] = {
    { 200, "application/json",
      "{\"type\":\"object\","
      "\"properties\":{"
      "\"lwip_max_sockets\":{\"type\":\"integer\"},"
      "\"in_use\":{\"type\":\"integer\"},"
      "\"by_state\":{\"type\":\"object\",\"properties\":{"
      "\"CLOSED\":{\"type\":\"integer\"},"
      "\"LISTEN\":{\"type\":\"integer\"},"
      "\"SYN_SENT\":{\"type\":\"integer\"},"
      "\"SYN_RCVD\":{\"type\":\"integer\"},"
      "\"ESTABLISHED\":{\"type\":\"integer\"},"
      "\"FIN_WAIT_1\":{\"type\":\"integer\"},"
      "\"FIN_WAIT_2\":{\"type\":\"integer\"},"
      "\"CLOSE_WAIT\":{\"type\":\"integer\"},"
      "\"CLOSING\":{\"type\":\"integer\"},"
      "\"LAST_ACK\":{\"type\":\"integer\"},"
      "\"TIME_WAIT\":{\"type\":\"integer\"}},"
      "\"required\":[\"CLOSED\",\"LISTEN\",\"SYN_SENT\",\"SYN_RCVD\",\"ESTABLISHED\","
      "\"FIN_WAIT_1\",\"FIN_WAIT_2\",\"CLOSE_WAIT\",\"CLOSING\",\"LAST_ACK\",\"TIME_WAIT\"]},"
      "\"pcbs\":{\"type\":\"array\",\"items\":{"
      "\"type\":\"object\","
      "\"properties\":{"
      "\"local_port\":{\"type\":\"integer\"},"
      "\"remote_ip\":{\"type\":\"string\"},"
      "\"remote_port\":{\"type\":\"integer\"},"
      "\"state\":{\"type\":\"string\"}}}}}}",
      "LWIP TCP socket state distribution: per-state counts and per-PCB detail "
      "(B1-1190: by_state schema tightened from additionalProperties to the 11 "
      "named required integer fields this route always emits -- runtime JSON "
      "is unchanged)" },
    { 0 },
};

static const bb_route_t s_sockets_get_route = {
    .method    = BB_HTTP_GET,
    .path      = "/api/diag/sockets",
    .tag       = "diag",
    .summary   = "LWIP TCP socket state distribution (in_use, by_state counts, per-PCB detail)",
    .responses = s_sockets_get_responses,
    .handler   = sockets_get_handler,
};

bb_err_t bb_diag_routes_init(bb_http_handle_t server)
{
    if (!server) return BB_ERR_INVALID_ARG;

    bb_err_t err = bb_http_register_described_route(server, &s_boot_get_route);
    if (err != BB_OK) return err;

    err = bb_http_register_described_route(server, &s_boot_delete_route);
    if (err != BB_OK) return err;

    err = bb_http_register_described_route(server, &s_panic_get_route);
    if (err != BB_OK) return err;

#ifdef CONFIG_BB_DIAG_PANIC_TRIGGER
    err = bb_http_register_described_route(server, &s_panic_trigger_route);
    if (err != BB_OK) return err;
    bb_log_w(TAG, "panic trigger route ENABLED — debug build only");
#endif

#ifdef CONFIG_BB_DIAG_PANIC_COREDUMP
    err = bb_http_register_described_route(server, &s_coredump_get_route);
    if (err != BB_OK) return err;
#endif

    err = bb_http_register_described_route(server, &s_heap_check_get_route);
    if (err != BB_OK) return err;

#if CONFIG_FREERTOS_USE_TRACE_FACILITY
    err = bb_http_register_described_route(server, &s_tasks_get_route);
    if (err != BB_OK) return err;
#endif

    err = bb_http_register_described_route(server, &s_sockets_get_route);
    if (err != BB_OK) return err;

    // Latch the reboot-reason SSOT once, before the first diag_boot_publish().
    load_reboot_record();

    // Register diag.boot in bb_cache (owned struct). SSE/broadcast delivery
    // is a bb_data/bb_data_http composition-root concern (B1-1045); the REST
    // GET path is now bb_data too (B1-1053 PR1) -- bb_cache here is purely
    // the snapshot store bb_diag_boot_gather() reads via bb_cache_get_raw(),
    // no .serialize slot, BB_CACHE_FLAG_NONE, no event topic.
    {
        bb_cache_config_t cache_cfg = {
            .key       = BB_DIAG_BOOT_TOPIC,
            .snapshot  = NULL,
            .snap_size = sizeof(bb_diag_boot_snap_t),
            .flags     = BB_CACHE_FLAG_NONE,
        };
        bb_err_t cerr = bb_cache_register(&cache_cfg);
        if (cerr != BB_OK) {
            bb_log_w(TAG, "bb_cache_register diag.boot failed: %d", (int)cerr);
        }
    }

    // Bind "diag.boot" to bb_data so boot_get_handler's bb_data_render() call
    // can resolve it -- no composition root does this today (see
    // bb_diag_boot_wire.h's file-header note), so this component self-binds,
    // mirroring bb_ota_check_config_bind()'s pattern. SSE/broadcast
    // attach (bb_data_http_attach_sized()) stays a composition-root concern,
    // out of this PR's scope.
    {
        bb_err_t derr = bb_diag_boot_bind();
        if (derr != BB_OK) {
            bb_log_w(TAG, "bb_diag_boot_bind failed: %d", (int)derr);
        }
    }

    // Publish the initial snapshot and register the OpenAPI schema for the
    // "diag.boot" bb_data key.
    {
        static const char k_diag_boot_schema[] =
            "{\"title\":\"DiagBoot\",\"x-sse-topic\":\"diag.boot\",\"type\":\"object\","
            "\"properties\":{"
            "\"reset_reason\":{\"type\":\"string\"},"
            "\"wdt_resets\":{\"type\":\"integer\"},"
            "\"panic\":{\"type\":\"object\",\"properties\":{"
            "\"available\":{\"type\":\"boolean\"},"
            "\"boots_since\":{\"type\":\"integer\"}}},"
            "\"pending_verify\":{\"type\":\"boolean\"},"
            "\"rolled_back\":{\"type\":\"boolean\"},"
            "\"reboot_reason\":{\"type\":\"object\",\"properties\":{"
            "\"source\":{\"type\":\"string\"},"
            "\"detail\":{\"type\":\"string\"},"
            "\"uptime_s\":{\"type\":\"integer\"},"
            "\"epoch_s\":{\"type\":\"integer\"},"
            "\"age_s\":{\"type\":\"integer\"}}},"
            "\"reboot_history\":{\"type\":\"array\",\"items\":{\"type\":\"object\",\"properties\":{"
            "\"source\":{\"type\":\"string\"},"
            "\"epoch_s\":{\"type\":\"integer\"},"
            "\"uptime_s\":{\"type\":\"integer\"}}}}},"
            "\"required\":[\"reset_reason\",\"wdt_resets\",\"panic\","
            "\"pending_verify\",\"rolled_back\",\"reboot_reason\",\"reboot_history\"]}";

        bb_openapi_register_topic_schema(BB_DIAG_BOOT_TOPIC, k_diag_boot_schema, "DiagBoot");

        // Publish initial retained snapshot and wire the on_validated callback
        // so the snapshot re-touches when the image self-validates.
        diag_boot_publish();
        bb_ota_validator_set_on_validated(diag_boot_publish);
    }

    bb_log_i(TAG, "diag routes registered");
    return BB_OK;
}

