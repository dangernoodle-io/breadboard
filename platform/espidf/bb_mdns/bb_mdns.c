#include "bb_mdns.h"
#include "bb_wifi.h"
#include "mdns.h"
#include "esp_app_desc.h"
#include "esp_mac.h"
#include "board.h"
#include "log_stream.h"
#include <string.h>

static const char *TAG = "bb_mdns";

// App-injected mDNS hostname
static char s_mdns_hostname[64] = "bsp-device";
static bool s_mdns_hostname_set = false;
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
    err = mdns_instance_name_set("BSP Device");
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

    err = mdns_service_add(NULL, "_bsp", "_tcp", 80, txt, 3);
    if (err != ESP_OK) {
        bb_log_e(TAG, "mdns_service_add failed: %s", esp_err_to_name(err));
        return;
    }
    s_mdns_started = true;

    bb_log_i(TAG, "mDNS started: %s.local (_bsp._tcp)", hostname);
}

// Callback invoked by bb_wifi when IP is obtained
static void bb_mdns_on_got_ip(void)
{
    bb_mdns_start_internal();
    if (s_mdns_started) {
        mdns_instance_name_set("BSP Device");
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

bool bb_mdns_started(void)
{
    return s_mdns_started;
}
