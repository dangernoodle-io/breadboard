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

/* JSON-Schema value for the health.display SSE topic (B1-1179: corrected
 * from a spurious "panel":["string","null"] nullable union -- the
 * bb_display_info_wire_desc serializer never emits a JSON null; "panel"
 * (and every other field but "present") carries a `.present` predicate
 * (display_info_present(), bb_display_info_wire.c) that OMITS the key
 * entirely from the rendered object when no display backend is present
 * -- see bb_serialize_walk.c's `if (f->present && !f->present(snap))
 * continue;`. So "panel" is a plain optional string, matching the
 * "present" field's meta-engine-generated companion below.
 * "required":["present"] + "additionalProperties":false brought into line
 * with the meta engine's fixed object-schema shape (B1-1059 PR-2b-i
 * convention; see bb_display_info_wire.c's co-located
 * bb_display_info_wire_meta banner for the golden byte-fidelity proof). */
static const char k_display_schema[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"present\":{\"type\":\"boolean\"},"
    "\"panel\":{\"type\":\"string\"},"
    "\"width\":{\"type\":\"integer\"},"
    "\"height\":{\"type\":\"integer\"},"
    "\"enabled\":{\"type\":\"boolean\"}},"
    "\"required\":[\"present\"],"
    "\"additionalProperties\":false}";

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

    bb_openapi_register_topic_schema(BB_DISPLAY_INFO_TOPIC, k_display_schema, "DisplayInfo");

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
