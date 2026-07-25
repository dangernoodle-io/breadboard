#include "bb_display_info.h"
#include "bb_display_info_event_priv.h"
#include "bb_display_info_wire.h"
#include "bb_cache.h"
#include "bb_data.h"
#include "bb_display.h"
#include "bb_log.h"
#include "bb_data_http.h"
#include "bb_http_server.h"
#include "bb_settings.h"
#include "bb_str.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

static const char *TAG = "bb_display";
static bool s_registered = false;

static bb_display_snap_t make_snap(void)
{
    bb_display_snap_t snap = {0};
    const char *panel = bb_display_backend_name();
    snap.present = (panel != NULL);
    if (panel) {
        bb_strlcpy(snap.panel, panel, sizeof(snap.panel));
        snap.width   = bb_display_width();
        snap.height  = bb_display_height();
        snap.enabled = bb_settings_display_enabled_get();
    }
    return snap;
}

// ---------------------------------------------------------------------------
// bb_display_register_info: register the health.display bb_cache entry +
// OpenAPI topic schema.
//
// Must be called before the deferred registry-tier init below.
//
// B1-893: re-homed from the deleted bb_display_info satellite -- this
// cache/SSE surface is independent of bb_info and stays live. The
// /api/info "display" section (bb_info_register_section) died with the
// satellite; only the cache/openapi registration below survives.
// ---------------------------------------------------------------------------

void bb_display_register_info(void)
{
    // Register owned-struct cache entry first (bb_display_info_gather()
    // reads it via bb_cache_get_raw()). SSE/broadcast delivery is a
    // bb_data/bb_data_http composition-root concern now (B1-1045), not
    // bb_cache's -- BB_CACHE_FLAG_NONE. cfg->serialize is intentionally
    // omitted (B1-1146a: the legacy bb_json bb_cache serializer,
    // bb_display_serialize(), is deleted -- health.display is being
    // rehomed to system.display under bb_system's diag endpoint, B1-1150,
    // which is where the REST read lives going forward, not here).
    bb_cache_config_t cache_cfg = {
        .key       = BB_DISPLAY_INFO_TOPIC,
        .snapshot  = NULL,
        .snap_size = sizeof(bb_display_snap_t),
        .flags     = BB_CACHE_FLAG_NONE,
    };
    bb_err_t cerr = bb_cache_register(&cache_cfg);
    if (cerr != BB_OK) {
        bb_log_w(TAG, "bb_cache_register failed: %d", (int)cerr);
        return;
    }

    // Describe the "health.display" bb_cache key via the B1-1220
    // bb_data_http_describe() seam (bb_display no longer links bb_openapi at
    // all, see CMakeLists.txt). key and topic are both BB_DISPLAY_INFO_TOPIC.
    // A composition root that wants "health.display" back in
    // /api/openapi.json must wire
    // bb_openapi_set_topic_source_fn(bb_data_http_describe_foreach) -- see
    // bb_openapi.h's seam doc.
    //
    // Doc-only bookkeeping (feeds /api/openapi.json schema synthesis) -- a
    // compose failure here must not abort bring-up: schema composition is
    // documentation-only and must never block the bb_data bind +
    // s_registered flag below. Degrade and continue -- log a warning and
    // fall through, skipping the describe call entirely (never describe
    // with an empty/poisoned schema buffer). The bb_data_http_describe()
    // call itself is also non-fatal on failure: it logs a warning and
    // falls through rather than aborting bb_display_register_info(), since
    // its backing table (BB_DATA_HTTP_MAX_DESCRIBE) is shared, first-come,
    // no-eviction, and can legitimately be full by the time this producer
    // registers.
#if defined(CONFIG_BB_OPENAPI_RUNTIME_META)
    bb_err_t schema_rc = bb_display_info_ensure_schema_patched();
    if (schema_rc != BB_OK) {
        bb_log_w(TAG, "health.display schema compose failed: %d", (int)schema_rc);
    } else {
        bb_err_t describe_rc = bb_data_http_describe(BB_DISPLAY_INFO_TOPIC, BB_DISPLAY_INFO_TOPIC,
                                                      "DisplayInfo", bb_display_info_get_schema());
        if (describe_rc != BB_OK) {
            bb_log_w(TAG, "health.display schema describe failed: %d", (int)describe_rc);
        }
    }
#else
    bb_err_t describe_rc = bb_data_http_describe(BB_DISPLAY_INFO_TOPIC, BB_DISPLAY_INFO_TOPIC,
                                                  "DisplayInfo", bb_display_info_get_schema());
    if (describe_rc != BB_OK) {
        bb_log_w(TAG, "health.display schema describe failed: %d", (int)describe_rc);
    }
#endif /* CONFIG_BB_OPENAPI_RUNTIME_META */

    // Bind "health.display" to bb_data (B1-1146a) so a future REST/SSE
    // reader (B1-1119/B1-1150: bb_system's diag endpoint, once
    // health.display is rehomed to system.display) can resolve it via
    // bb_data_render(). Non-fatal like bb_diag_boot_bind()'s call site: a
    // bind failure (e.g. BB_DATA_MAX_BINDINGS already full) leaves the key
    // unbound until re-bound, but does not block the rest of this
    // registration.
    bb_err_t derr = bb_display_info_bind();
    if (derr != BB_OK) {
        bb_log_w(TAG, "bb_display_info_bind failed: %d", (int)derr);
    }

    s_registered = true;
}

// ---------------------------------------------------------------------------
// bb_display_info_register_init: deferred registry-tier init (order 4).
//
// Seeds the initial cache snapshot and bumps the "health.display" bb_data
// generation (B1-1045) -- attach/wiring to /api/events lives at the
// composition root.
// ---------------------------------------------------------------------------

bb_err_t bb_display_info_register_init(bb_http_handle_t server)
{
    (void)server;
    if (!s_registered) return BB_OK;

    bb_display_snap_t snap = make_snap();
    bb_cache_update(&(bb_cache_update_t){ .key = BB_DISPLAY_INFO_TOPIC, .snap = &snap });
    bb_data_touch(BB_DISPLAY_INFO_TOPIC);

    return BB_OK;
}
