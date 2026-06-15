#include "bb_thermal.h"
#include "bb_fan.h"
#include "bb_fan_test.h"
#include "bb_power.h"
#include "bb_power_test.h"
#include "bb_temp.h"
#include "bb_http_extender.h"
#include "bb_json.h"
#include <math.h>
#include <stdbool.h>
#include <string.h>

// Host twin of the /api/thermal route — provides schema assembly and testing hooks.

// ---------------------------------------------------------------------------
// Internal helper: emit {present, c} sub-object (mirrors espidf emit_source).
// ---------------------------------------------------------------------------

static void host_emit_source(bb_json_t root, const char *key, bool present, double c_val)
{
    bb_json_t obj = bb_json_obj_new();
    bb_json_obj_set_bool(obj, "present", present);
    if (present) {
        bb_json_obj_set_number(obj, "c", c_val);
    } else {
        bb_json_obj_set_null(obj, "c");
    }
    bb_json_obj_set_obj(root, key, obj);
}

// ---------------------------------------------------------------------------
// Shared emit helper — host implementation mirrors espidf (SSOT).
// ---------------------------------------------------------------------------

void bb_thermal_emit_section(bb_json_t obj)
{
    float soc_c = 0.0f;
    bool soc_present = bb_temp_read_soc(&soc_c);

    bb_power_handle_t pwr = bb_power_primary();
    bb_power_snapshot_t psnap;
    bb_power_snapshot(pwr, &psnap);
    bool vr_present = (pwr != NULL && psnap.temp_c >= 0);

    bb_fan_handle_t fan = bb_fan_primary();
    bb_fan_snapshot_t fsnap;
    bb_fan_snapshot(fan, &fsnap);
    bool asic_present  = (fan != NULL && !isnan(fsnap.die_c));
    bool board_present = (fan != NULL && !isnan(fsnap.board_c));

    host_emit_source(obj, "soc",   soc_present,  soc_present   ? (double)soc_c        : 0.0);
    host_emit_source(obj, "vr",    vr_present,   vr_present    ? (double)psnap.temp_c  : 0.0);
    host_emit_source(obj, "asic",  asic_present, asic_present  ? (double)fsnap.die_c   : 0.0);
    host_emit_source(obj, "board", board_present,board_present ? (double)fsnap.board_c  : 0.0);
}

static const char k_thermal_schema_base[] =
    "{\"type\":\"object\","
    "\"properties\":{"
    "\"soc\":{\"type\":\"object\",\"properties\":{"
    "\"present\":{\"type\":\"boolean\"},"
    "\"c\":{\"type\":[\"number\",\"null\"]}}},"
    "\"vr\":{\"type\":\"object\",\"properties\":{"
    "\"present\":{\"type\":\"boolean\"},"
    "\"c\":{\"type\":[\"number\",\"null\"]}}},"
    "\"asic\":{\"type\":\"object\",\"properties\":{"
    "\"present\":{\"type\":\"boolean\"},"
    "\"c\":{\"type\":[\"number\",\"null\"]}}},"
    "\"board\":{\"type\":\"object\",\"properties\":{"
    "\"present\":{\"type\":\"boolean\"},"
    "\"c\":{\"type\":[\"number\",\"null\"]}}}";

static const char k_thermal_schema_suffix[] =
    "},"
    "\"required\":[\"soc\",\"vr\",\"asic\",\"board\"]}";

#ifdef BB_THERMAL_TESTING

const char *bb_thermal_get_assembled_schema(void)
{
    const char *cached = bb_http_extender_get_assembled_schema("thermal");
    if (cached) return cached;
    return bb_http_route_assemble_schema("thermal", k_thermal_schema_base, k_thermal_schema_suffix);
}

void bb_thermal_reset_for_test(void)
{
    bb_temp_test_set_soc(false, 0.0f);
    bb_fan_test_reset();
    bb_power_test_reset();
    // extender reset is handled globally by bb_info_reset_for_test → bb_http_extender_reset_for_test
}

#endif /* BB_THERMAL_TESTING */
