#include "bb_mdns.h"
#include "bb_wifi.h"
#include "mdns.h"
#include "esp_app_desc.h"
#include "esp_mac.h"
#include "bb_hw.h"
#include "bb_log.h"
#include <string.h>

static const char *TAG = "bb_mdns";

// App-injected mDNS hostname
static char s_mdns_hostname[64] = "bsp-device";
static bool s_mdns_hostname_set = false;

// App-injected mDNS service type and instance name
static char s_mdns_service_type[32] = "_bsp";
static bool s_mdns_service_type_set = false;
static char s_mdns_instance_name[64] = "BSP Device";
static bool s_mdns_instance_name_set = false;

static bool s_mdns_started = false;

static void mdns_build_hostname(char *out, size_t out_size)
{
    if (s_mdns_hostname_set && s_mdns_hostname[0] != '\0') {
        snprintf(out, out_size, "%s", s_mdns_hostname);
    } else {
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        snprintf(out, out_size, "bsp-device-%02x%02x", mac[4], mac[5]);
    }

    /* mDNS label max 63 chars */
    out[63] = '\0';
}

static void bb_mdns_start_internal(void)
{
    if (s_mdns_started) {
        return;
    }

    char hostname[64];
    mdns_build_hostname(hostname, sizeof(hostname));

    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        bb_log_e(TAG, "mdns_init failed: %s", esp_err_to_name(err));
        return;
    }

    err = mdns_hostname_set(hostname);
    if (err != ESP_OK) {
        bb_log_e(TAG, "mdns_hostname_set failed: %s", esp_err_to_name(err));
        return;
    }
    const char *instance_name = s_mdns_instance_name_set ? s_mdns_instance_name : "BSP Device";
    err = mdns_instance_name_set(instance_name);
    if (err != ESP_OK) {
        bb_log_e(TAG, "mdns_instance_name_set failed: %s", esp_err_to_name(err));
        return;
    }

    const esp_app_desc_t *app = esp_app_get_description();
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    char mac_str[13];
    snprintf(mac_str, sizeof(mac_str), "%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    mdns_txt_item_t txt[] = {
        {"board",   BOARD_NAME},
        {"version", app->version},
        {"mac",     mac_str},
    };

    const char *service_type = s_mdns_service_type_set ? s_mdns_service_type : "_bsp";
    err = mdns_service_add(NULL, service_type, "_tcp", 80, txt, 3);
    if (err != ESP_OK) {
        bb_log_e(TAG, "mdns_service_add failed: %s", esp_err_to_name(err));
        return;
    }
    s_mdns_started = true;

    bb_log_i(TAG, "mDNS started: %s.local (%s._tcp)", hostname, service_type);
}

// Callback invoked by bb_wifi when IP is obtained
static void bb_mdns_on_got_ip(void)
{
    bb_mdns_start_internal();
    if (s_mdns_started) {
        const char *instance_name = s_mdns_instance_name_set ? s_mdns_instance_name : "BSP Device";
        mdns_instance_name_set(instance_name);
    }
}

void bb_mdns_init(void)
{
    // Register callback with bb_wifi
    bb_wifi_register_on_got_ip(bb_mdns_on_got_ip);
}

void bb_mdns_set_hostname(const char *hostname)
{
    if (!hostname) {
        s_mdns_hostname[0] = '\0';
        s_mdns_hostname_set = false;
        return;
    }
    strncpy(s_mdns_hostname, hostname, sizeof(s_mdns_hostname) - 1);
    s_mdns_hostname[sizeof(s_mdns_hostname) - 1] = '\0';
    s_mdns_hostname_set = true;
}

void bb_mdns_set_service_type(const char *service_type)
{
    if (!service_type) {
        s_mdns_service_type[0] = '\0';
        s_mdns_service_type_set = false;
        return;
    }
    strncpy(s_mdns_service_type, service_type, sizeof(s_mdns_service_type) - 1);
    s_mdns_service_type[sizeof(s_mdns_service_type) - 1] = '\0';
    s_mdns_service_type_set = true;
}

void bb_mdns_set_instance_name(const char *instance_name)
{
    if (!instance_name) {
        s_mdns_instance_name[0] = '\0';
        s_mdns_instance_name_set = false;
        return;
    }
    strncpy(s_mdns_instance_name, instance_name, sizeof(s_mdns_instance_name) - 1);
    s_mdns_instance_name[sizeof(s_mdns_instance_name) - 1] = '\0';
    s_mdns_instance_name_set = true;
}

bool bb_mdns_started(void)
{
    return s_mdns_started;
}
