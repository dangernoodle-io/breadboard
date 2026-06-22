// bb_sensors host twin — test hooks for /api/sensors.
// Provides section registry, schema assembly, and test-isolation reset.
#include "bb_sensors.h"
#include "bb_section.h"
#include "bb_fan.h"
#include "bb_json.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "../../../components/bb_sensors/bb_sensors_schema_priv.h"

// File-scope section registry for /api/sensors.
static bb_section_registry_t s_sensors_reg = { .tag = "bb_sensors" };

// Cached assembled schema (lazy, freed on reset).
static char *s_assembled_schema = NULL;

#ifdef BB_SENSORS_TESTING
#include "bb_sensors.h"
#endif

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bb_err_t bb_sensors_register_section(const char *name,
                                      bb_section_get_fn get,
                                      bb_section_patch_fn patch,
                                      void *ctx,
                                      const char *schema_props)
{
    return bb_section_register(&s_sensors_reg, name, get, patch, ctx, schema_props);
}

// ---------------------------------------------------------------------------
// Test hooks
// ---------------------------------------------------------------------------

#ifdef BB_SENSORS_TESTING

void bb_sensors_freeze_for_test(void)
{
    bb_section_freeze(&s_sensors_reg);
}

void bb_sensors_invoke_sections_for_test(bb_json_t root)
{
    bb_section_build_get(&s_sensors_reg, root);
}

bb_err_t bb_sensors_dispatch_patch_for_test(bb_json_t body)
{
    return bb_section_dispatch_patch(&s_sensors_reg, body);
}

void bb_sensors_reset_for_test(void)
{
    memset(&s_sensors_reg, 0, sizeof(s_sensors_reg));
    s_sensors_reg.tag = "bb_sensors";
    free(s_assembled_schema);
    s_assembled_schema = NULL;
}

const char *bb_sensors_get_assembled_schema(void)
{
    if (!s_assembled_schema) {
        s_assembled_schema = bb_section_freeze_and_assemble(&s_sensors_reg, k_sensors_base, k_sensors_suffix);
    }
    return s_assembled_schema;
}

// Test hook: drive the real fan autofan validation logic (mirrors fan_section_patch
// in platform/espidf/bb_sensors/bb_sensors.c) without needing an HTTP server.
bb_err_t bb_sensors_fan_patch_for_test(bb_json_t patch_body)
{
    bb_fan_handle_t h = bb_fan_primary();
    if (!h) return BB_ERR_INVALID_STATE;

#ifdef CONFIG_BB_FAN_AUTOFAN
    bb_fan_autofan_cfg_t cfg;
    bb_fan_get_autofan_cfg(h, &cfg);

    double d;
    bool b;
    if (bb_json_obj_get_number(patch_body, "manual_pct", &d)) {
        if (d < 0.0 || d > 100.0) return BB_ERR_INVALID_ARG;
        cfg.manual_pct = (int)d;
    }
    if (bb_json_obj_get_number(patch_body, "min_pct", &d)) {
        if (d < 0.0 || d > 100.0) return BB_ERR_INVALID_ARG;
        cfg.min_pct = (int)d;
    }
    if (bb_json_obj_get_number(patch_body, "die_target_c", &d)) {
        if (d <= 0.0) return BB_ERR_INVALID_ARG;
        cfg.die_target_c = (float)d;
    }
    if (bb_json_obj_get_number(patch_body, "vr_target_c", &d)) {
        if (d <= 0.0) return BB_ERR_INVALID_ARG;
        cfg.aux_target_c = (float)d;
    }
    if (bb_json_obj_get_bool(patch_body, "autofan", &b)) cfg.enabled = b;
    bb_fan_set_autofan(h, &cfg);
    return BB_OK;
#else
    double duty_d = -1.0;
    if (!bb_json_obj_get_number(patch_body, "duty_pct", &duty_d)) {
        return BB_ERR_INVALID_ARG;
    }
    int duty = (int)duty_d;
    if (duty < 0 || duty > 100) return BB_ERR_INVALID_ARG;
    bb_fan_set_duty_pct(h, duty);
    return BB_OK;
#endif
}

#endif /* BB_SENSORS_TESTING */
