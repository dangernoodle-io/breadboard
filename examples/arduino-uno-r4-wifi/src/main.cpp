#include <Arduino.h>
#include "bb_log.h"
#include "bb_nv.h"
#include "bb_http.h"
#include "bb_wifi.h"
#include "secrets.h"

static const char *TAG = "bb-uno-r4";

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

    uint32_t boot_count = 0;
    bb_nv_get_u32("app", "boot", &boot_count, 0);
    boot_count++;
    bb_nv_set_u32("app", "boot", boot_count);
    bb_log_i(TAG, "boot=%lu", (unsigned long)boot_count);

    bb_wifi_set_hostname("bb-uno-r4");
    if (bb_wifi_init_sta() != BB_OK) {
        bb_log_e(TAG, "wifi assoc failed");
        while (1);
    }

    bb_http_server_start();
    bb_http_register_route(bb_http_server_get_handle(), BB_HTTP_GET, "/ping", ping_handler);
}

void loop() {
    bb_http_server_poll();
    delay(10);
}
