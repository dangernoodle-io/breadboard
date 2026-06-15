// bb_telemetry — section registry + build/dispatch helpers.
// Compiled on both host (test) and ESP-IDF.
//
// Internally delegates to bb_section; public API is ABI-identical to before.
#include "bb_telemetry.h"
#include "bb_section.h"
#include "bb_log.h"

#include <stddef.h>
#include <string.h>

static const char *TAG = "bb_telemetry";

#ifndef CONFIG_BB_TELEMETRY_MAX_SECTIONS
#define CONFIG_BB_TELEMETRY_MAX_SECTIONS 4
#endif

// Verify that the configured section capacity fits in bb_section's registry.
// Both constants default to ≤ BB_SECTION_MAX (4 ≤ 8), so this is a safety net.
#if CONFIG_BB_TELEMETRY_MAX_SECTIONS > BB_SECTION_MAX
#error "CONFIG_BB_TELEMETRY_MAX_SECTIONS exceeds BB_SECTION_MAX"
#endif

static bb_section_registry_t s_reg = { .tag = "bb_telemetry" };
static bool  s_pending_reboot = false;

bb_err_t bb_telemetry_register_section_ex(const char *name,
                                           bb_telemetry_get_fn get,
                                           bb_telemetry_patch_fn patch,
                                           void *ctx,
                                           const char *schema_props)
{
    if (!name || !get) return BB_ERR_INVALID_ARG;
    if (s_reg.count >= CONFIG_BB_TELEMETRY_MAX_SECTIONS) {
        bb_log_w(TAG, "register_section('%s'): registry full (max %d)", name,
                 CONFIG_BB_TELEMETRY_MAX_SECTIONS);
        return BB_ERR_NO_SPACE;
    }
    bb_err_t rc = bb_section_register(&s_reg, name, get, patch, ctx, schema_props);
    if (rc == BB_OK) {
        bb_log_d(TAG, "registered section '%s' (%s)", name, patch ? "rw" : "ro");
    }
    return rc;
}

bb_err_t bb_telemetry_register_section(const char *name,
                                        bb_telemetry_get_fn get,
                                        bb_telemetry_patch_fn patch,
                                        void *ctx)
{
    return bb_telemetry_register_section_ex(name, get, patch, ctx, NULL);
}

void bb_telemetry_freeze(void)
{
    bb_section_freeze(&s_reg);
}

void bb_telemetry_build_get(bb_json_t root)
{
    bb_section_build_get(&s_reg, root);
}

bb_err_t bb_telemetry_dispatch_patch(bb_json_t body)
{
    // Delegate to bb_section_dispatch_patch for consistent pre-validation
    // (all-or-nothing: read-only section in body → reject before any apply).
    bb_err_t rc = bb_section_dispatch_patch(&s_reg, body);
    if (rc != BB_OK) {
        bb_log_w(TAG, "dispatch_patch failed: %d", (int)rc);
        return rc;
    }

    // Set pending_reboot only when at least one section was actually patched.
    bool any_patched = false;
    for (int i = 0; i < s_reg.count; i++) {
        if (bb_json_obj_get_item(body, s_reg.entries[i].name)) {
            any_patched = true;
            break;
        }
    }
    if (any_patched) {
        s_pending_reboot = true;
        bb_log_i(TAG, "telemetry config patched; reboot required");
    }
    return BB_OK;
}

bool bb_telemetry_pending_reboot(void)
{
    return s_pending_reboot;
}

// ---------------------------------------------------------------------------
// Schema assembly — builds the real composed GET schema from per-section props.
// ---------------------------------------------------------------------------

char *bb_telemetry_assemble_get_schema(void)
{
    // Returns a freshly allocated string — caller owns and must free.
    return bb_section_assemble_schema(
        &s_reg,
        "{\"type\":\"object\",\"properties\":{",
        "}}");
}

#ifdef BB_TELEMETRY_TESTING

void bb_telemetry_reset_for_test(void)
{
    memset(&s_reg, 0, sizeof(s_reg));
    s_reg.tag        = "bb_telemetry";
    s_pending_reboot = false;
}

void bb_telemetry_build_get_for_test(bb_json_t root)
{
    bb_telemetry_build_get(root);
}

bb_err_t bb_telemetry_dispatch_patch_for_test(bb_json_t body)
{
    return bb_telemetry_dispatch_patch(body);
}

#endif /* BB_TELEMETRY_TESTING */
