// bb_sensors_dispatch -- binds fan/power/thermal into bb_data and registers
// the shared "/api/sensors/" bb_http_section namespace over them (bb_sensors
// PR-2, B1-828 epic).
//
// FIXED SERVABLE SET, NOT A GLOBAL bb_data PROXY: bb_data_bind() is a single
// flat, PROCESS-WIDE table with no namespace concept -- every key any
// component has bound (e.g. examples/floor's "log"/"diag.meminfo"/
// "diag.system") lives in the same table as fan/power/thermal. sensors_render()/
// sensors_apply() below therefore resolve the URI's "<name>" segment against
// the fixed {fan, power, thermal} set FIRST (sensors_key_is_known()) and
// return BB_ERR_NOT_FOUND for anything else, before ever calling into
// bb_data -- so /api/sensors/<key> can only ever reach the three keys this
// component itself bound, never an unrelated key bound by some other
// composition-root consumer. Mirrors bb_diag_section's own fixed-registry
// posture (components/bb_diag/bb_diag_section.c); see bb_http_section.h's
// "REGISTRY-AGNOSTIC BY DESIGN" note for why bb_http_section itself stays
// bb_data-free at the routing seam -- this fixed-set check is what keeps
// that seam from silently becoming an unbounded bb_data-key surface.
//
// Compiles on both host and ESP-IDF -- bb_data_render()/bb_data_parse()/
// bb_data_commit()/bb_http_section_register_ns() are all portable; only the
// route registration itself (bb_http_section_init(), called separately by
// bb_sensors_init()) is ESP-IDF only.
#include "bb_sensors_dispatch_priv.h"
#include "bb_sensors_wire_priv.h"

#include "bb_data.h"
#include "bb_http_section.h"

#include <stddef.h>
#include <string.h>

// Scratch sizing: the largest bound wire struct is bb_sensors_fan_wire_t
// (autofan variant, 5 fields) or bb_sensors_thermal_wire_t (4 nested
// {bool,double} sources) -- both comfortably under 64 bytes. Generous
// headroom over a tight sizeof() so a future field addition doesn't need a
// matching bump here.
#define BB_SENSORS_SCRATCH_BYTES  64

// bb_data_parse()'s parse_scratch backs bb_serialize_json_parse_bytes()'s
// token-recorder control structs + token pool, NOT just the decoded field
// bytes -- mirrors bb_wifi_http_routes.c's WIFI_PATCH_PARSE_SCRATCH_BYTES /
// bb_storage_http_routes.c's FACTORY_RESET_PARSE_SCRATCH_BYTES, the same
// 3072-byte sizing every other bb_data-backed PATCH route in this codebase
// uses (a few hundred bytes undersizes it -- BB_ERR_NO_SPACE at PARSE stage).
#define BB_SENSORS_PARSE_SCRATCH_BYTES  3072

// Fixed servable set -- see the file header's "FIXED SERVABLE SET" note.
// name == bb_data key 1:1 for exactly these three; anything else is
// resolved to BB_ERR_NOT_FOUND before touching bb_data.
static bool sensors_key_is_known(const char *name)
{
    return name != NULL
        && (strcmp(name, "fan") == 0 || strcmp(name, "power") == 0 || strcmp(name, "thermal") == 0);
}

// Shared render hook -- resolves `name` against the fixed set above, then
// proxies to bb_data_render() with `name` (the URI segment bb_http_section
// already stripped down to) as the bb_data key.
static bb_err_t sensors_render(const char *name, const bb_serialize_query_t *query,
                                char *buf, size_t cap, size_t *out_len, void *ctx)
{
    (void)ctx;

    if (!sensors_key_is_known(name)) return BB_ERR_NOT_FOUND;

    char scratch[BB_SENSORS_SCRATCH_BYTES];
    bb_data_render_req_t req = {
        .fmt         = BB_FORMAT_JSON,
        .key         = name,
        .query       = query,
        .scratch     = scratch,
        .scratch_cap = sizeof(scratch),
        .buf         = buf,
        .buf_cap     = cap,
        .out_len     = out_len,
    };
    return bb_data_render(&req);
}

// Shared apply hook -- drives bb_data_parse()/bb_data_commit() DIRECTLY (not
// bb_data_apply()) so the stage tag bb_http_section_status_for_apply() needs
// is authoritative rather than guessed (mirrors
// test_bb_http_section_e2e_apply_drives_bb_data_parse_and_commit, PR-1's own
// proof this shape is real). BB_DATA_APPLY_PATCH: a wire field absent from
// the request body keeps whatever the binding's gather() hook seeded it
// with (see bb_sensors_fan_gather()'s own PATCH-seed contract doc).
static bb_http_section_apply_result_t sensors_apply(const char *name, const char *body,
                                                      size_t body_len, void *ctx)
{
    (void)ctx;

    if (!sensors_key_is_known(name)) {
        return (bb_http_section_apply_result_t){ .stage = BB_HTTP_SECTION_STAGE_PARSE,
                                                   .rc    = BB_ERR_NOT_FOUND };
    }

    char parse_scratch[BB_SENSORS_PARSE_SCRATCH_BYTES];
    bb_data_parse_req_t parse_req = {
        .fmt               = BB_FORMAT_JSON,
        .key               = name,
        .body              = body,
        .body_len          = body_len,
        .parse_scratch     = parse_scratch,
        .parse_scratch_cap = sizeof(parse_scratch),
    };
    bb_data_parsed_t parsed;
    bb_err_t rc = bb_data_parse(&parse_req, &parsed);
    if (rc != BB_OK) {
        return (bb_http_section_apply_result_t){ .stage = BB_HTTP_SECTION_STAGE_PARSE, .rc = rc };
    }

    char dst_scratch[BB_SENSORS_SCRATCH_BYTES];
    bb_data_commit_req_t commit_req = {
        .mode            = BB_DATA_APPLY_PATCH,
        .dst_scratch     = dst_scratch,
        .dst_scratch_cap = sizeof(dst_scratch),
    };
    rc = bb_data_commit(&parsed, &commit_req);
    return (bb_http_section_apply_result_t){ .stage = BB_HTTP_SECTION_STAGE_COMMIT, .rc = rc };
}

bb_err_t bb_sensors_bind_and_register(void)
{
    bb_data_binding_t fan_binding = {
        .key    = "fan",
        .desc   = &bb_sensors_fan_wire_desc,
        .gather = bb_sensors_fan_gather,
        .apply  = bb_sensors_fan_apply,
    };
    bb_err_t err = bb_data_bind(&fan_binding);
    if (err != BB_OK) return err;

    bb_data_binding_t power_binding = {
        .key    = "power",
        .desc   = &bb_sensors_power_wire_desc,
        .gather = bb_sensors_power_gather,
        // apply == NULL: egress-only -- PATCH /api/sensors/power -> 405.
    };
    err = bb_data_bind(&power_binding);
    if (err != BB_OK) return err;

    bb_data_binding_t thermal_binding = {
        .key    = "thermal",
        .desc   = &bb_sensors_thermal_wire_desc,
        .gather = bb_sensors_thermal_gather,
        // apply == NULL: egress-only -- PATCH /api/sensors/thermal -> 405.
    };
    err = bb_data_bind(&thermal_binding);
    if (err != BB_OK) return err;

    bb_http_section_ns_t ns = {
        .prefix = "/api/sensors/",
        .render = sensors_render,
        .apply  = sensors_apply,
        // 503, not the mapper's 405 default: a commit-stage BB_ERR_UNSUPPORTED
        // on this namespace can ONLY come from bb_sensors_fan_apply()'s own
        // "no primary fan" reject (see bb_sensors_wire.c) -- power/thermal
        // are apply-less bindings, so their PATCH rejection happens at
        // bb_data_parse()'s PARSE stage instead, which the status mapper
        // hardcodes to 405 regardless of this override (see
        // bb_http_section_status_for_apply()). "no primary fan" is an
        // ordinary, possibly-transient hardware state, not "this namespace
        // doesn't support writes" -- 503 (service unavailable) restores the
        // pre-PR-2 handler's semantics, mirrors bb_diag_http's (formerly
        // bb_storage_http's, B1-1154) / bb_wifi_http's own unsupported_status
        // precedent (see bb_http_section_ns_t's doc).
        .unsupported_status = 503,
    };
    return bb_http_section_register_ns(&ns);
}
