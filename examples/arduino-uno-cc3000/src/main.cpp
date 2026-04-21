#include <Arduino.h>
#include <Adafruit_CC3000.h>
#include "bb_log.h"
#include "bb_nv.h"
#include "bb_http.h"
#include "secrets.h"

static const char *TAG = "bb-uno";

// Adafruit CC3000 shield pin mapping (both v1.0 and v1.1 shields)
static Adafruit_CC3000 cc3000(10 /*CS*/, 3 /*IRQ*/, 5 /*VBAT*/,
                              SPI_CLOCK_DIVIDER);

// Handler for GET /ping
static bb_err_t ping_handler(bb_http_request_t *req) {
    bb_http_resp_set_header(req, "Content-Type", "text/plain");
    bb_http_resp_send(req, "pong\n", 5);
    bb_log_i(TAG, "GET /ping -> pong");
    return BB_OK;
}


void setup() {
    Serial.begin(115200);
    while (!Serial);

    bb_nv_config_init();
    bb_log_i(TAG, "boot");

    // Round-trip a value through bb_nv_* to prove EEPROM backend works.
    uint32_t boot_count = 0;
    bb_nv_get_u32("app", "boot", &boot_count, 0);
    boot_count++;
    bb_nv_set_u32("app", "boot", boot_count);
    bb_log_i(TAG, "boot=%lu", (unsigned long)boot_count);

    bb_log_i(TAG, "cc3000 begin...");
    if (!cc3000.begin()) {
        bb_log_e(TAG, "cc3000 begin failed");
        while (1);
    }
    bb_log_i(TAG, "cc3000 begin ok");

    uint8_t major, minor;
    if (!cc3000.getFirmwareVersion(&major, &minor)) {
        bb_log_e(TAG, "fw version read failed");
    } else {
        bb_log_i(TAG, "fw %u.%u", major, minor);
    }

    if (!cc3000.connectToAP(WIFI_SSID, WIFI_PASS, WLAN_SEC_WPA2)) {
        bb_log_e(TAG, "wifi assoc failed");
        while (1);
    }

    while (!cc3000.checkDHCP()) {
        delay(100);
    }

    // checkDHCP() can return true before the lease is actually populated.
    // Poll getIPAddress() until it returns a non-zero IP.
    uint32_t ip = 0, nm, gw, dhcp, dns;
    while (true) {
        if (cc3000.getIPAddress(&ip, &nm, &gw, &dhcp, &dns) && ip != 0) {
            break;
        }
        delay(250);
    }
    bb_log_i(TAG, "ip %lu.%lu.%lu.%lu",
             (unsigned long)((ip >> 24) & 0xff),
             (unsigned long)((ip >> 16) & 0xff),
             (unsigned long)((ip >> 8) & 0xff),
             (unsigned long)(ip & 0xff));

    bb_log_i(TAG, "online");

    // Start HTTP server
    bb_http_server_start();
    bb_http_register_route(bb_http_server_get_handle(), BB_HTTP_GET, "/ping", ping_handler);
}

void loop() {
    // Service HTTP requests
    bb_http_server_poll();
    delay(10);
}
