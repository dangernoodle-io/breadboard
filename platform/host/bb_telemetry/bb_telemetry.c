// bb_telemetry — section registry + build/dispatch helpers.
// Compiled on both host (test) and ESP-IDF.
//
// Internally delegates to bb_response; public API is ABI-identical to before.
#include "bb_telemetry.h"
#include "bb_response.h"
#include "bb_log.h"
#include "bb_nv.h"
#include "bb_nv_namespaces.h"
#include "bb_pub.h"
#include "bb_json.h"

#include <stddef.h>
#include <string.h>

// Key for each sink's enabled flag. bb_telemetry deliberately does NOT
// REQUIRES bb_mqtt / bb_sink_http (it only reads each sink's persisted
// "enabled" flag via the generic bb_nv API); the namespace strings
// themselves come from the SSOT header (bb_nv_namespaces.h) via bb_nv.
#define BB_TELEMETRY_SINK_NVS_KEY    "enabled"

static const char *TAG = "bb_telemetry";

#ifndef CONFIG_BB_TELEMETRY_MAX_SECTIONS
#define CONFIG_BB_TELEMETRY_MAX_SECTIONS 4
#endif

// Verify that the configured section capacity fits in bb_response's registry.
// Both constants default to ≤ BB_RESPONSE_MAX (4 ≤ 8), so this is a safety net.
#if CONFIG_BB_TELEMETRY_MAX_SECTIONS > BB_RESPONSE_MAX
#error "CONFIG_BB_TELEMETRY_MAX_SECTIONS exceeds BB_RESPONSE_MAX"
#endif

static bb_response_registry_t s_reg = { .tag = "bb_telemetry",
                                         .cap = CONFIG_BB_TELEMETRY_MAX_SECTIONS };
static bool  s_pending_reboot = false;

bb_err_t bb_telemetry_register_section_ex(const char *name,
                                           bb_telemetry_get_fn get,
                                           bb_telemetry_patch_fn patch,
                                           void *ctx,
                                           const char *schema_props)
{
    if (!name || !get) return BB_ERR_INVALID_ARG;
    // Capacity gate is in bb_response_register (s_reg.cap == CONFIG_BB_TELEMETRY_MAX_SECTIONS).
    // The compile-time #error above already guarantees TELEMETRY_MAX <= BB_RESPONSE_MAX.
    bb_err_t rc = bb_response_register(&s_reg, name, get, patch, ctx, schema_props);
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
    bb_response_freeze(&s_reg);
}

void bb_telemetry_build_get(bb_json_t root)
{
    bb_response_build_get(&s_reg, root);
}

// ---------------------------------------------------------------------------
// Publisher–sink coupling helper (pure, host-testable)
// ---------------------------------------------------------------------------

// Determine the publisher enabled value to persist after a PATCH that may
// have changed sink enabled flags.
//
// Semantics:
//   - If publisher_explicit is true, the caller's body explicitly set
//     publisher.enabled — honour publisher_explicit_value directly (override).
//   - Otherwise auto-couple: publisher enabled = any_sink_enabled.
//
// Returns the value that bb_pub_set_enabled should be called with.
bool bb_telemetry_couple_publisher(bool any_sink_enabled,
                                   bool publisher_explicit,
                                   bool publisher_explicit_value)
{
    if (publisher_explicit) {
        return publisher_explicit_value;
    }
    return any_sink_enabled;
}

bb_err_t bb_telemetry_dispatch_patch(bb_json_t body)
{
    // Snapshot sink enabled states BEFORE applying the patch so we can
    // detect which sinks the body changed.
    char mqtt_pre[4] = "0";
    char http_pre[4] = "0";
    bb_nv_get_str(BB_MQTT_NVS_NS, BB_TELEMETRY_SINK_NVS_KEY,
                  mqtt_pre, sizeof(mqtt_pre), "0");
    bb_nv_get_str(BB_SINK_HTTP_NVS_NS, BB_TELEMETRY_SINK_NVS_KEY,
                  http_pre, sizeof(http_pre), "0");

    // Delegate to bb_response_dispatch_patch for consistent pre-validation
    // (all-or-nothing: read-only section in body → reject before any apply).
    bb_err_t rc = bb_response_dispatch_patch(&s_reg, body);
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

    // ---------------------------------------------------------------------------
    // Publisher–sink coupling (B1-pub-sink-coupling).
    //
    // If the body changed a sink's enabled field (mqtt or http section present),
    // sync the publisher's persisted enabled flag to (any sink enabled), unless
    // the same body ALSO explicitly set publisher.enabled (user override wins).
    // ---------------------------------------------------------------------------
    bool mqtt_section_present = (bb_json_obj_get_item(body, "mqtt") != NULL);
    bool http_section_present = (bb_json_obj_get_item(body, "http") != NULL);

    // Did the body change any sink's enabled state?
    bool sink_enabled_changed = false;
    if (mqtt_section_present || http_section_present) {
        // Read post-patch sink enabled state from NVS.
        char mqtt_post[4] = "0";
        char http_post[4] = "0";
        bb_nv_get_str(BB_MQTT_NVS_NS, BB_TELEMETRY_SINK_NVS_KEY,
                      mqtt_post, sizeof(mqtt_post), "0");
        bb_nv_get_str(BB_SINK_HTTP_NVS_NS, BB_TELEMETRY_SINK_NVS_KEY,
                      http_post, sizeof(http_post), "0");

        // Coupling triggers when any sink enabled flag was written (changed or
        // re-written) by the patch, i.e. the mqtt or http section was present
        // AND contained an "enabled" key — inferred from pre vs post comparison
        // or by checking body sub-objects directly.
        bool mqtt_enabled_in_patch = false;
        bool http_enabled_in_patch = false;
        if (mqtt_section_present) {
            bb_json_t mqtt_sub = bb_json_obj_get_item(body, "mqtt");
            bool dummy = false;
            mqtt_enabled_in_patch = bb_json_obj_get_bool(mqtt_sub, "enabled", &dummy);
        }
        if (http_section_present) {
            bb_json_t http_sub = bb_json_obj_get_item(body, "http");
            bool dummy = false;
            http_enabled_in_patch = bb_json_obj_get_bool(http_sub, "enabled", &dummy);
        }

        sink_enabled_changed = (mqtt_enabled_in_patch || http_enabled_in_patch);

        if (sink_enabled_changed) {
            bool mqtt_now = (mqtt_post[0] == '1');
            bool http_now = (http_post[0] == '1');
            bool any_sink_enabled = (mqtt_now || http_now);

            // Check if the same body explicitly set publisher.enabled.
            bool pub_explicit = false;
            bool pub_explicit_value = false;
            bb_json_t pub_sub = bb_json_obj_get_item(body, "publisher");
            if (pub_sub) {
                pub_explicit = bb_json_obj_get_bool(pub_sub, "enabled",
                                                    &pub_explicit_value);
            }

            bool new_enabled = bb_telemetry_couple_publisher(
                any_sink_enabled, pub_explicit, pub_explicit_value);
            bb_err_t cerr = bb_pub_set_enabled(new_enabled);
            if (cerr != BB_OK) {
                bb_log_w(TAG, "couple_publisher: bb_pub_set_enabled failed: %d",
                         (int)cerr);
            } else {
                bb_log_i(TAG, "coupled publisher enabled=%s (mqtt=%s http=%s%s)",
                         new_enabled ? "true" : "false",
                         mqtt_now   ? "true" : "false",
                         http_now   ? "true" : "false",
                         pub_explicit ? " [explicit override]" : "");
            }
        }
    }
    (void)mqtt_pre;
    (void)http_pre;

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
    return bb_response_freeze_and_assemble(
        &s_reg,
        "{\"type\":\"object\",\"properties\":{",
        "}}");
}

#ifdef BB_TELEMETRY_TESTING

void bb_telemetry_reset_for_test(void)
{
    memset(&s_reg, 0, sizeof(s_reg));
    s_reg.tag        = "bb_telemetry";
    s_reg.cap        = CONFIG_BB_TELEMETRY_MAX_SECTIONS;
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
