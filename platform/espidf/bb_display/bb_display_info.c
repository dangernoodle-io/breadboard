#include "bb_display_info.h"
#include "bb_display_info_event_priv.h"
#include "bb_display_info_wire.h"
#include "bb_cache.h"
#include "bb_data.h"
#include "bb_display.h"
#include "bb_log.h"
#include "bb_openapi.h"
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

    // Register the OpenAPI schema for the "health.display" bb_cache key.
    // Compose-then-register (B1-1059 SSE PR-4): the hand literal moved to
    // bb_display_info_wire.c (relocation, see its own banner) -- config-OFF
    // this register call serves that literal unchanged; config-ON, ensure
    // the schema is composed first (fail-loud) before serving the
    // runtime-composed buffer.
#if defined(CONFIG_BB_OPENAPI_RUNTIME_META)
    bb_err_t schema_rc = bb_display_info_ensure_schema_patched();
    if (schema_rc != BB_OK) {
        bb_log_w(TAG, "health.display schema compose failed: %d", (int)schema_rc);
        return;
    }
#endif /* CONFIG_BB_OPENAPI_RUNTIME_META */
    bb_openapi_register_topic_schema(BB_DISPLAY_INFO_TOPIC, bb_display_info_get_schema(), "DisplayInfo");

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
