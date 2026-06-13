// bb_mqtt_telemetry host twin — section get/patch logic + test hooks.
// Compiled on both host (test) and ESP-IDF (shared logic).
#include "bb_mqtt_telemetry.h"
#include "bb_mqtt.h"
#include "bb_nv.h"
#include "bb_json.h"
#include "bb_telemetry.h"
#include "bb_registry.h"
#include "bb_pub.h"
#include "bb_log.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

// Stable string ID used for the exclusive-sink arbiter.
#define BB_SINK_MQTT_EXCLUSIVE_ID  "mqtt"

static const char *TAG = "bb_mqtt_telemetry";

#define BB_MQTT_NVS_NS      "bb_mqtt"
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

    char ca_probe[8]   = {0};
    char cert_probe[8] = {0};
    char key_probe[8]  = {0};
    bb_nv_get_str(BB_MQTT_NVS_NS, "tls_ca",   ca_probe,   sizeof(ca_probe),   "");
    bb_nv_get_str(BB_MQTT_NVS_NS, "tls_cert", cert_probe, sizeof(cert_probe), "");
    bb_nv_get_str(BB_MQTT_NVS_NS, "tls_key",  key_probe,  sizeof(key_probe),  "");

    bool ca_set   = (ca_probe[0]   != '\0');
    bool cert_set = (cert_probe[0] != '\0');
    bool key_set  = (key_probe[0]  != '\0');
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

    char tmp[BB_MQTT_BODY_MAX + 1];

    if (bb_json_obj_get_string(patch, "uri",       tmp, sizeof(tmp))) {
        bb_nv_set_str(BB_MQTT_NVS_NS, "uri", tmp);
    }
    if (bb_json_obj_get_string(patch, "client_id", tmp, sizeof(tmp))) {
        bb_nv_set_str(BB_MQTT_NVS_NS, "client_id", tmp);
    }
    if (bb_json_obj_get_string(patch, "username",  tmp, sizeof(tmp))) {
        bb_nv_set_str(BB_MQTT_NVS_NS, "username", tmp);
    }
    if (bb_json_obj_get_string(patch, "password",  tmp, sizeof(tmp))) {
        bb_nv_set_str(BB_MQTT_NVS_NS, "password", tmp);
    }
    if (bb_json_obj_get_string(patch, "tls_ca",   tmp, sizeof(tmp))) {
        bb_nv_set_str(BB_MQTT_NVS_NS, "tls_ca", tmp);
    }
    if (bb_json_obj_get_string(patch, "tls_cert", tmp, sizeof(tmp))) {
        bb_nv_set_str(BB_MQTT_NVS_NS, "tls_cert", tmp);
    }
    if (bb_json_obj_get_string(patch, "tls_key",  tmp, sizeof(tmp))) {
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
                return arc;
            }
        } else {
            bb_pub_exclusive_release(BB_SINK_MQTT_EXCLUSIVE_ID);
        }
        bb_nv_set_str(BB_MQTT_NVS_NS, "enabled", b ? "1" : "0");
    }

    // Apply the new config to the live client without a reboot.
    bb_mqtt_reconfigure();

    return BB_OK;
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------

bb_err_t bb_mqtt_telemetry_init(void)
{
    // Boot-time precedence: if this sink is NVS-enabled, acquire the exclusive
    // slot. If another sink already holds the slot (invalid dual-enabled NVS
    // state), log a warning, write enabled=0 back to NVS, and leave this sink
    // inactive. MQTT wins over HTTP because it is registered first (PRE_HTTP
    // tier ordering).
    char enabled_str[4] = "0";
    bb_nv_get_str(BB_MQTT_NVS_NS, "enabled", enabled_str, sizeof(enabled_str), "0");
    if (enabled_str[0] == '1') {
        bb_err_t arc = bb_pub_exclusive_acquire(BB_SINK_MQTT_EXCLUSIVE_ID);
        if (arc != BB_OK) {
            /* Should not happen: MQTT is the first PRE_HTTP registrant and
             * always wins the slot. Log and disable to leave a clean state. */
            bb_log_w(TAG, "boot: exclusive slot already taken — "
                     "disabling mqtt sink");
            bb_nv_set_str(BB_MQTT_NVS_NS, "enabled", "0");
        }
    }
    return bb_telemetry_register_section("mqtt", mqtt_section_get, mqtt_section_patch, NULL);
}

#if CONFIG_BB_MQTT_TELEMETRY_AUTOREGISTER
BB_REGISTRY_REGISTER_PRE_HTTP(bb_mqtt_telemetry, bb_mqtt_telemetry_init);
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
