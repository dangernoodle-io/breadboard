// bb_telemetry — section registry + build/dispatch helpers.
// Compiled on both host (test) and ESP-IDF.
#include "bb_telemetry.h"
#include "bb_log.h"

#include <stddef.h>
#include <string.h>

static const char *TAG = "bb_telemetry";

#ifndef CONFIG_BB_TELEMETRY_MAX_SECTIONS
#define CONFIG_BB_TELEMETRY_MAX_SECTIONS 4
#endif

typedef struct {
    const char             *name;
    bb_telemetry_get_fn     get;
    bb_telemetry_patch_fn   patch;
    void                   *ctx;
} bb_telemetry_section_t;

static bb_telemetry_section_t s_sections[CONFIG_BB_TELEMETRY_MAX_SECTIONS];
static int  s_count           = 0;
static bool s_pending_reboot  = false;

bb_err_t bb_telemetry_register_section(const char *name,
                                        bb_telemetry_get_fn get,
                                        bb_telemetry_patch_fn patch,
                                        void *ctx)
{
    if (!name || !get) return BB_ERR_INVALID_ARG;
    if (s_count >= CONFIG_BB_TELEMETRY_MAX_SECTIONS) return BB_ERR_NO_SPACE;
    s_sections[s_count].name  = name;
    s_sections[s_count].get   = get;
    s_sections[s_count].patch = patch;
    s_sections[s_count].ctx   = ctx;
    s_count++;
    bb_log_d(TAG, "registered section '%s' (%s)", name, patch ? "rw" : "ro");
    return BB_OK;
}

// ---------------------------------------------------------------------------
// Build GET / dispatch PATCH — always compiled (called by route + test hooks)
// ---------------------------------------------------------------------------

void bb_telemetry_build_get(bb_json_t root)
{
    for (int i = 0; i < s_count; i++) {
        bb_json_t child = bb_json_obj_new();
        s_sections[i].get(child, s_sections[i].ctx);
        bb_json_obj_set_obj(root, s_sections[i].name, child);
    }
}

bb_err_t bb_telemetry_dispatch_patch(bb_json_t body)
{
    bool any_ok = false;
    for (int i = 0; i < s_count; i++) {
        bb_json_t child = bb_json_obj_get_item(body, s_sections[i].name);
        if (!child) continue;
        if (!s_sections[i].patch) {
            bb_log_w(TAG, "PATCH on read-only section '%s'", s_sections[i].name);
            return BB_ERR_INVALID_ARG;
        }
        bb_err_t rc = s_sections[i].patch(child, s_sections[i].ctx);
        if (rc != BB_OK) return rc;
        any_ok = true;
    }
    if (any_ok) {
        s_pending_reboot = true;
    }
    return BB_OK;
}

bool bb_telemetry_pending_reboot(void)
{
    return s_pending_reboot;
}

#ifdef BB_TELEMETRY_TESTING

void bb_telemetry_reset_for_test(void)
{
    memset(s_sections, 0, sizeof(s_sections));
    s_count          = 0;
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
