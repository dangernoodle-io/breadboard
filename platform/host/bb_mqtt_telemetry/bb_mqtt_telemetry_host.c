// bb_mqtt_telemetry host twin — section get/patch logic + test hooks.
// Compiled on both host (test) and ESP-IDF (shared logic).
//
// B1-289: PATCH /api/telemetry validates + writes NVS only (no live reconfigure).
// Boot: bb_mqtt_telemetry_init wires the ONE enabled sink at PRE_HTTP time.
#include "bb_mqtt_telemetry.h"
#include "bb_mqtt.h"
#include "bb_sink_mqtt.h"
#include "bb_nv.h"
#include "bb_json.h"
#include "bb_telemetry.h"
#include "bb_init.h"
#include "bb_pub.h"
#include "bb_log.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

// Stable string ID used for the exclusive-sink arbiter.
#define BB_SINK_MQTT_EXCLUSIVE_ID  "mqtt"

static const char *TAG = "bb_mqtt_telemetry";

// BB_MQTT_NVS_NS is the SSOT namespace constant from bb_mqtt.h.
#define BB_MQTT_STR_MAX     64
#define BB_MQTT_BODY_MAX    4096

static bb_mqtt_t *s_client_ref = NULL;

void bb_mqtt_telemetry_set_client(bb_mqtt_t *ref)
{
    s_client_ref = ref;
}

// ---------------------------------------------------------------------------
// Section get
// ---------------------------------------------------------------------------

static void mqtt_section_get(bb_json_t section, void *ctx)
{
    (void)ctx;

    char uri[BB_MQTT_STR_MAX]       = {0};
    char client_id[BB_MQTT_STR_MAX] = {0};
    char username[BB_MQTT_STR_MAX]  = {0};
    char password[BB_MQTT_STR_MAX]  = {0};
    char enabled_str[4]             = "0";
    char tls_str[4]                 = "0";

    bb_nv_get_str(BB_MQTT_NVS_NS, "uri",       uri,         sizeof(uri),         "");
    bb_nv_get_str(BB_MQTT_NVS_NS, "client_id", client_id,   sizeof(client_id),   "");
    bb_nv_get_str(BB_MQTT_NVS_NS, "username",  username,    sizeof(username),    "");
    bb_nv_get_str(BB_MQTT_NVS_NS, "password",  password,    sizeof(password),    "");
    bb_nv_get_str(BB_MQTT_NVS_NS, "enabled",   enabled_str, sizeof(enabled_str), "0");
    bb_nv_get_str(BB_MQTT_NVS_NS, "tls",       tls_str,     sizeof(tls_str),     "0");

    bool ca_set   = bb_nv_exists(BB_MQTT_NVS_NS, "tls_ca");
    bool cert_set = bb_nv_exists(BB_MQTT_NVS_NS, "tls_cert");
    bool key_set  = bb_nv_exists(BB_MQTT_NVS_NS, "tls_key");
    bool tls_on   = (tls_str[0] == '1');
    bool enabled  = (enabled_str[0] == '1');

    bool connected = false;
    if (s_client_ref && *s_client_ref) {
        connected = bb_mqtt_is_connected(*s_client_ref);
    } else {
        bb_mqtt_t def = bb_mqtt_default();
        if (def) connected = bb_mqtt_is_connected(def);
    }

    bb_json_obj_set_string(section, "uri",       uri);
    bb_json_obj_set_string(section, "client_id", client_id);
    bb_json_obj_set_string(section, "username",  username);
    bb_json_obj_set_string(section, "password",  password[0] ? "***" : "");
    bb_json_obj_set_bool  (section, "tls",       tls_on);
    bb_json_obj_set_bool  (section, "ca_set",    ca_set);
    bb_json_obj_set_bool  (section, "cert_set",  cert_set);
    bb_json_obj_set_bool  (section, "key_set",   key_set);
    bb_json_obj_set_bool  (section, "enabled",   enabled);
    bb_json_obj_set_bool  (section, "connected", connected);
}

// ---------------------------------------------------------------------------
// Section patch
// ---------------------------------------------------------------------------

static bb_err_t mqtt_section_patch(bb_json_t patch, void *ctx)
{
    (void)ctx;

    // Heap-allocate the scratch buffer — 4 KB on the stack overflowed the
    // 6144-byte httpd thread stack when combined with cJSON parse frames,
    // causing a 'stack overflow in task httpd' panic on bitaxe.
    char *tmp = malloc((size_t)BB_MQTT_BODY_MAX + 1);
    if (!tmp) return BB_ERR_NO_SPACE;

    if (bb_json_obj_get_string(patch, "uri",       tmp, BB_MQTT_BODY_MAX + 1)) {
        bb_nv_set_str(BB_MQTT_NVS_NS, "uri", tmp);
    }
    if (bb_json_obj_get_string(patch, "client_id", tmp, BB_MQTT_BODY_MAX + 1)) {
        bb_nv_set_str(BB_MQTT_NVS_NS, "client_id", tmp);
    }
    if (bb_json_obj_get_string(patch, "username",  tmp, BB_MQTT_BODY_MAX + 1)) {
        bb_nv_set_str(BB_MQTT_NVS_NS, "username", tmp);
    }
    if (bb_json_obj_get_string(patch, "password",  tmp, BB_MQTT_BODY_MAX + 1)) {
        bb_nv_set_str(BB_MQTT_NVS_NS, "password", tmp);
    }
    if (bb_json_obj_get_string(patch, "tls_ca",   tmp, BB_MQTT_BODY_MAX + 1)) {
        bb_nv_set_str(BB_MQTT_NVS_NS, "tls_ca", tmp);
    }
    if (bb_json_obj_get_string(patch, "tls_cert", tmp, BB_MQTT_BODY_MAX + 1)) {
        bb_nv_set_str(BB_MQTT_NVS_NS, "tls_cert", tmp);
    }
    if (bb_json_obj_get_string(patch, "tls_key",  tmp, BB_MQTT_BODY_MAX + 1)) {
        bb_nv_set_str(BB_MQTT_NVS_NS, "tls_key", tmp);
    }

    bool b;
    if (bb_json_obj_get_bool(patch, "tls", &b)) {
        bb_nv_set_str(BB_MQTT_NVS_NS, "tls", b ? "1" : "0");
    }
    if (bb_json_obj_get_bool(patch, "enabled", &b)) {
        if (b) {
            // Acquire the exclusive slot before persisting enabled=true.
            bb_err_t arc = bb_pub_exclusive_acquire(BB_SINK_MQTT_EXCLUSIVE_ID);
            if (arc != BB_OK) {
                bb_log_w(TAG, "enable rejected: exclusive sink conflict");
                free(tmp);
                return arc;
            }
        } else {
            bb_pub_exclusive_release(BB_SINK_MQTT_EXCLUSIVE_ID);
        }
        bb_nv_set_str(BB_MQTT_NVS_NS, "enabled", b ? "1" : "0");
    }

    free(tmp);

    // B1-289: NVS-only patch; live reconfigure removed. A reboot is required
    // for the new config to take effect (signalled by bb_telemetry_pending_reboot).
    return BB_OK;
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------

// Static sink struct: lives for the app lifetime once wired at boot.
static bb_pub_sink_t s_mqtt_sink;

bb_err_t bb_mqtt_telemetry_init(void)
{
    // Boot-time precedence: first PRE_HTTP registrant wins the exclusive slot.
    // In breadboard's default registration order MQTT registers before HTTP,
    // so MQTT wins by default.  Consumer apps that register HTTP first get HTTP
    // as the winner.  If this sink is NVS-enabled, try to acquire the slot.
    char enabled_str[4] = "0";
    bb_nv_get_str(BB_MQTT_NVS_NS, "enabled", enabled_str, sizeof(enabled_str), "0");
    if (enabled_str[0] == '1') {
        bb_err_t arc = bb_pub_exclusive_acquire(BB_SINK_MQTT_EXCLUSIVE_ID);
        if (arc == BB_OK) {
            // WINNER: wire a dynamic sink that resolves bb_mqtt_default() at
            // publish time so it survives the OTA suspend/resume client-recreate.
            // The handle is NOT stable across suspend/resume — capturing it at
            // boot was a use-after-free (B1-296). bb_sink_mqtt_default() resolves
            // bb_mqtt_default() on every publish call; a NULL handle (suspend
            // window) yields a clean no-op rather than dereferencing freed memory.
            if (bb_sink_mqtt_default(&s_mqtt_sink) == BB_OK) {
                bb_pub_add_sink(&s_mqtt_sink);
                bb_log_i(TAG, "boot: mqtt sink registered (winner, dynamic handle)");
            } else {
                bb_log_w(TAG, "boot: mqtt enabled but sink init failed");
            }
        } else {
            // LOSER: another sink won. Write enabled=0 for a consistent next
            // boot and stop the EARLY-connected auto-client to free its heap
            // immediately (no reboot required — B1-289 loser teardown).
            bb_log_w(TAG, "boot: exclusive slot already taken — "
                     "disabling mqtt sink and stopping auto-client");
            bb_nv_set_str(BB_MQTT_NVS_NS, "enabled", "0");
            bb_mqtt_stop_default();
        }
    }
    static const char k_mqtt_schema_props[] =
        "{\"type\":\"object\","
        "\"properties\":{"
        "\"uri\":{\"type\":\"string\"},"
        "\"client_id\":{\"type\":\"string\"},"
        "\"username\":{\"type\":\"string\"},"
        "\"password\":{\"type\":\"string\"},"
        "\"tls\":{\"type\":\"boolean\"},"
        "\"ca_set\":{\"type\":\"boolean\"},"
        "\"cert_set\":{\"type\":\"boolean\"},"
        "\"key_set\":{\"type\":\"boolean\"},"
        "\"enabled\":{\"type\":\"boolean\"},"
        "\"connected\":{\"type\":\"boolean\"}}}";
    return bb_telemetry_register_section_ex("mqtt", mqtt_section_get,
                                             mqtt_section_patch, NULL,
                                             k_mqtt_schema_props);
}

#if CONFIG_BB_MQTT_TELEMETRY_AUTOREGISTER
BB_INIT_REGISTER_PRE_HTTP(bb_mqtt_telemetry, bb_mqtt_telemetry_init);
#endif

// ---------------------------------------------------------------------------
// Test hooks
// ---------------------------------------------------------------------------

#ifdef BB_MQTT_TELEMETRY_TESTING

void bb_mqtt_telemetry_reset_for_test(void)
{
    s_client_ref = NULL;
    bb_nv_host_str_store_reset();
    bb_pub_exclusive_reset();
}

void bb_mqtt_telemetry_section_get_for_test(bb_json_t section, void *ctx)
{
    mqtt_section_get(section, ctx);
}

bb_err_t bb_mqtt_telemetry_section_patch_for_test(bb_json_t patch, void *ctx)
{
    return mqtt_section_patch(patch, ctx);
}

#endif /* BB_MQTT_TELEMETRY_TESTING */
