#include "bb_info.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "bb_board.h"
// ota_ready is emitted only on boards with a runtime heap-guarded OTA TLS path
// (pull worker or boot-mode on-demand check); gated so boot-only boards don't
// link bb_ota_pull just for this field. Keep in sync with bb_pub_info + CMakeLists.
#if (defined(CONFIG_BB_OTA_PULL_AUTOREGISTER) && CONFIG_BB_OTA_PULL_AUTOREGISTER) || \
    (defined(CONFIG_BB_OTA_BOOT_STATUS_HTTP) && CONFIG_BB_OTA_BOOT_STATUS_HTTP)
#define BB_INFO_EMIT_OTA_READY 1
#include "bb_ota_pull.h"
#endif
#include "bb_cache.h"
#include "bb_clock.h"
#include "bb_event.h"
#include "bb_event_routes.h"
#include "bb_http.h"
#include "bb_json.h"
#include "bb_log.h"
#include "bb_mdns.h"
#include "bb_ntp.h"
#include "bb_openapi.h"
#include "bb_registry.h"
#include "bb_section.h"
#include "bb_wifi.h"

#include "../../../components/bb_info/bb_info_schema_priv.h"
#include "../../../components/bb_info/src/bb_info_build_priv.h"

// Kconfig bridge for CONFIG_BB_INFO_BUILD_TOPIC
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#ifdef CONFIG_BB_INFO_BUILD_TOPIC
#define BB_INFO_BUILD_TOPIC_ENABLED CONFIG_BB_INFO_BUILD_TOPIC
#endif
#endif
#ifndef BB_INFO_BUILD_TOPIC_ENABLED
#define BB_INFO_BUILD_TOPIC_ENABLED 1
#endif

static const char *TAG = "bb_info";

// File-scope section registry for /api/info.
static bb_section_registry_t s_info_reg = { .tag = "bb_info" };

// bb_cache "build" topic: retained event topic handle.
#define BB_INFO_BUILD_TOPIC "build"
static bb_event_topic_t s_build_topic = NULL;

// ---------------------------------------------------------------------------
// Capability registry
// ---------------------------------------------------------------------------

static const char *s_capabilities[BB_INFO_MAX_CAPABILITIES];
static int         s_capability_count = 0;
static bool        s_cap_frozen       = false;

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bb_err_t bb_info_register_section(const char *name,
                                   bb_section_get_fn get,
                                   void *ctx,
                                   const char *schema_props)
{
    return bb_section_register(&s_info_reg, name, get, NULL, ctx, schema_props);
}

void bb_info_register_capability(const char *name)
{
    if (!name || !name[0]) return;
    if (s_cap_frozen) {
        bb_log_w(TAG, "bb_info_register_capability(%s): ignored after freeze", name);
        return;
    }
    // Dedup: ignore if already registered.
    for (int i = 0; i < s_capability_count; i++) {
        if (strcmp(s_capabilities[i], name) == 0) return;
    }
    if (s_capability_count >= BB_INFO_MAX_CAPABILITIES) {
        bb_log_w(TAG, "bb_info_register_capability(%s): registry full, dropping", name);
        return;
    }
    s_capabilities[s_capability_count++] = name;
}

// ---------------------------------------------------------------------------
// JSON helpers
// ---------------------------------------------------------------------------

static void add_board_fields(bb_json_t root, const bb_board_info_t *b)
{
    // Static build fields moved to the nested "build" subsection (B1-360).
    // Only dynamic/runtime fields remain at the root level.
    bb_json_obj_set_string(root, "mac", b->mac);
    bb_json_obj_set_string(root, "reset_reason", b->reset_reason);
    bb_json_obj_set_bool(root, "ota_validated", b->ota_validated);
#if BB_INFO_EMIT_OTA_READY
    bb_json_obj_set_bool(root, "ota_ready", bb_ota_pull_heap_ready());
#endif

    bb_json_t heap_internal = bb_json_obj_new();
    bb_json_obj_set_number(heap_internal, "free",          (double)bb_board_heap_internal_free());
    bb_json_obj_set_number(heap_internal, "total",         (double)bb_board_heap_internal_total());
    bb_json_obj_set_number(heap_internal, "min_free",      (double)bb_board_heap_minimum_ever());
    bb_json_obj_set_number(heap_internal, "largest_block", (double)bb_board_heap_internal_largest_free_block());
    bb_json_obj_set_obj(root, "heap_internal", heap_internal);

    bb_json_t heap_psram = bb_json_obj_new();
    bb_json_obj_set_number(heap_psram, "free",  (double)bb_board_psram_free());
    bb_json_obj_set_number(heap_psram, "total", (double)bb_board_psram_total());
    bb_json_obj_set_obj(root, "heap_psram", heap_psram);

    bb_json_t rtc = bb_json_obj_new();
    bb_json_obj_set_number(rtc, "used",  (double)bb_board_rtc_used());
    bb_json_obj_set_number(rtc, "total", (double)bb_board_rtc_total());
    bb_json_obj_set_obj(root, "rtc", rtc);

    bb_json_t static_dram = bb_json_obj_new();
    bb_json_obj_set_number(static_dram, "bytes", (double)bb_board_dram_static_bytes());
    bb_json_obj_set_obj(root, "static_dram", static_dram);
}

static void add_network_object(bb_json_t root, const bb_wifi_info_t *w)
{
    bb_json_t net = bb_json_obj_new();
    bb_wifi_emit_section(net, w);
    bb_json_obj_set_obj(root, "network", net);
}

// REST callback for /api/info "build" section: reads from the bb_cache snapshot.
static void build_section_get(bb_json_t section, void *ctx)
{
    (void)ctx;
    bb_cache_serialize_into(BB_INFO_BUILD_TOPIC, section);
}

// Serialize a bb_json_t tree and stream it via chunked transfer.
static bb_err_t send_json_tree(bb_http_request_t *req, bb_json_t root)
{
    char *str = bb_json_serialize(root);
    if (!str) return BB_ERR_NO_SPACE;
    bb_err_t err = bb_http_resp_set_type(req, "application/json");
    if (err == BB_OK) err = bb_http_resp_send_chunk(req, str, -1);
    if (err == BB_OK) err = bb_http_resp_send_chunk(req, NULL, 0);
    bb_json_free_str(str);
    return err;
}

static bb_err_t info_handler(bb_http_request_t *req)
{
    bb_board_info_t b;
    bb_wifi_info_t w;
    bb_board_get_info(&b);
    bb_wifi_get_info(&w);

    bb_json_t root = bb_json_obj_new();
    add_board_fields(root, &b);
    add_network_object(root, &w);

    // Add HTTP handler telemetry
    extern size_t bb_http_route_handler_count(void);
    extern size_t bb_http_route_handler_cap(void);
    bb_json_obj_set_number(root, "http_handler_count", (double)bb_http_route_handler_count());
    bb_json_obj_set_number(root, "http_handler_cap", (double)bb_http_route_handler_cap());

    // Uptime, boot epoch, time validity — same accessors as bb_pub_info (SSOT).
    int64_t uptime_ms = bb_clock_now_ms();
    bb_json_obj_set_number(root, "uptime_ms", (double)uptime_ms);

    bool   time_valid = false;
    int64_t boot_epoch_s = 0;
    if (bb_ntp_is_synced()) {
        time_t now = time(NULL);
        if (now >= (time_t)1704067200LL) {
            time_valid  = true;
            int64_t uptime_s = uptime_ms / 1000;
            boot_epoch_s = (int64_t)now - uptime_s;
        }
    }
    bb_json_obj_set_bool  (root, "time_valid",   time_valid);
    bb_json_obj_set_number(root, "boot_epoch_s", (double)boot_epoch_s);
    // time_source: parity with bb_pub_info telemetry (was published but absent here).
    bb_json_obj_set_string(root, "time_source", time_valid ? "sntp" : "none");

    // Hostname — same value as /api/health.network.mdns.
    const char *hostname = bb_mdns_get_hostname();
    if (hostname) {
        bb_json_obj_set_string(root, "hostname", hostname);
    } else {
        bb_json_obj_set_null(root, "hostname");
    }

    // Emit capabilities array (always present, even if empty)
    bb_json_t caps = bb_json_arr_new();
    for (int i = 0; i < s_capability_count; i++) {
        bb_json_arr_append_string(caps, s_capabilities[i]);
    }
    bb_json_obj_set_arr(root, "capabilities", caps);

    // Emit named sections (display, led, ntp, diag, ...)
    bb_section_build_get(&s_info_reg, root);

    bb_err_t err = send_json_tree(req, root);
    bb_json_free(root);
    return err;
}

// ---------------------------------------------------------------------------
// Route descriptor
// ---------------------------------------------------------------------------

static bb_route_response_t s_info_responses[] = {
    { 200, "application/json",
      NULL,  // filled by bb_section_assemble_schema() at init
      "full device info including board and network" },
    { 0 },
};

static const bb_route_t s_info_route = {
    .method    = BB_HTTP_GET,
    .path      = "/api/info",
    .tag       = "info",
    .summary   = "Get full device info",
    .responses = s_info_responses,
    .handler   = info_handler,
};

static bb_err_t bb_info_init(bb_http_handle_t server)
{
    if (!server) return BB_ERR_INVALID_ARG;
    // NOTE: freeze and schema assembly are deferred to bb_info_freeze_init (order 20)
    // so that section registrants (e.g. bb_diag_routes at order BB_DIAG_ROUTE_COUNT)
    // can call bb_info_register_section AFTER this init runs at order 2.
    bb_err_t err = bb_http_register_described_route(server, &s_info_route);
    if (err != BB_OK) return err;

    // Register the bb_cache "build" topic and seed it with current build data.
    bb_err_t cerr = bb_cache_register(BB_INFO_BUILD_TOPIC, NULL,
                                      sizeof(bb_info_build_snap_t),
                                      bb_info_build_emit);
    if (cerr != BB_OK) {
        bb_log_w(TAG, "bb_cache_register(build) failed: %d", (int)cerr);
    } else {
        bb_info_build_snap_t snap;
        bb_info_build_capture(&snap);
        bb_cache_update(BB_INFO_BUILD_TOPIC, &snap);

        // Register the event topic for SSE fan-out.
        bb_err_t terr = bb_event_topic_register(BB_INFO_BUILD_TOPIC, &s_build_topic);
        if (terr != BB_OK) {
            bb_log_w(TAG, "event_topic_register(build) failed: %d", (int)terr);
        } else {
            static const char k_build_info_schema[] =
                "{\"title\":\"BuildInfo\",\"x-sse-topic\":\"build\",\"type\":\"object\","
                "\"properties\":{"
                "\"version\":{\"type\":\"string\"},"
                "\"idf_version\":{\"type\":\"string\"},"
                "\"build_date\":{\"type\":\"string\"},"
                "\"build_time\":{\"type\":\"string\"},"
                "\"project_name\":{\"type\":\"string\"},"
                "\"chip_model\":{\"type\":\"string\"},"
                "\"chip_revision\":{\"type\":\"integer\"},"
                "\"cores\":{\"type\":\"integer\"},"
                "\"cpu_freq_mhz\":{\"type\":\"integer\"},"
                "\"flash_size\":{\"type\":\"integer\"},"
                "\"app_size\":{\"type\":\"integer\"},"
                "\"board\":{\"type\":\"string\"},"
                "\"app_sha256\":{\"type\":\"string\"}},"
                "\"required\":[\"version\",\"idf_version\",\"build_date\",\"build_time\","
                "\"project_name\",\"chip_model\",\"chip_revision\",\"cores\","
                "\"cpu_freq_mhz\",\"flash_size\",\"app_size\",\"board\",\"app_sha256\"]}";
            bb_openapi_register_topic_schema(BB_INFO_BUILD_TOPIC, k_build_info_schema, "BuildInfo");
#if BB_INFO_BUILD_TOPIC_ENABLED
            // Build payload estimate: all 13 fields.
            // ~{"version":"0.0.0","idf_version":"v5.3.1","build_date":"Jan  1 2025",
            //   "build_time":"12:00:00","project_name":"breadboard","chip_model":"ESP32",
            //   "chip_revision":3,"cores":2,"cpu_freq_mhz":240,"flash_size":4194304,
            //   "app_size":1200000,"board":"wroom32","app_sha256":"deadbeef0"}
            // Key names (92) + values (83) + JSON overhead (brackets, quotes, colons, commas ~80) ≈ 255
            // Worst-case with long dev version + board name like "taipanminer-tdongle-s3" → ~320 B
            // Use 512 B max_entry (see update.available precedent from #562)
            bb_err_t aerr = bb_event_routes_attach_ex2(BB_INFO_BUILD_TOPIC, /*retained=*/true, 512);
            if (aerr != BB_OK) {
                bb_log_w(TAG, "attach_ex2(build) failed: %d", (int)aerr);
            }
#endif
            // Seed the ring so SSE clients connecting before the first poll get data.
            bb_cache_post(BB_INFO_BUILD_TOPIC);
        }
    }

    // Register build as a named section on /api/info (REST path reads from bb_cache).
    bb_info_register_section(BB_INFO_BUILD_TOPIC, build_section_get, NULL, k_build_schema);

    bb_log_i(TAG, "info route registered (freeze deferred to order 20)");
    return BB_OK;
}

// Late init: freeze the section registry and assemble the schema.
// Runs at order 20, AFTER all section registrants (display/led/ntp/diag at
// BB_DIAG_ROUTE_COUNT ≈ 7) have called bb_info_register_section.
static bb_err_t bb_info_freeze_init(bb_http_handle_t server)
{
    (void)server;
    s_cap_frozen = true;
    s_info_responses[0].schema = bb_section_freeze_and_assemble(&s_info_reg, k_info_schema_base, k_info_schema_suffix);
    bb_log_i(TAG, "info registry frozen (%d sections)", s_info_reg.count);
    return BB_OK;
}

#if CONFIG_BB_INFO_AUTOREGISTER
// PRE_HTTP companion: declare route count before server starts (must match
// the number of bb_http_register_* calls in bb_info_init above: 1).
static bb_err_t bb_info_reserve_routes(void)
{
    bb_http_reserve_routes(1);  // GET /api/info
    return BB_OK;
}
BB_REGISTRY_REGISTER_PRE_HTTP(bb_info, bb_info_reserve_routes);
BB_REGISTRY_REGISTER_N(bb_info, bb_info_init, 2);
BB_REGISTRY_REGISTER_N(bb_info_freeze, bb_info_freeze_init, 20);
#endif
