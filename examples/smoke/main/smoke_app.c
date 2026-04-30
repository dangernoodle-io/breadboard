// Portable smoke app — exercises bb_log, bb_nv, bb_wifi, bb_http on every
// supported framework/backend. Entry shims (entry_arduino.cpp / entry_espidf.c)
// call into smoke_app_setup() and smoke_app_loop().
//
// The same source compiles unchanged across all envs in platformio.ini.
// Backend selection happens via build_src_filter and build_flags.

#include "bb_log.h"
#include "bb_nv.h"
#include "bb_http.h"
#include "bb_wifi.h"
#include "smoke_app.h"

#ifdef BB_SMOKE_DISPLAY
#include "bb_display.h"
#endif

static const char *TAG = "smoke";

static bb_err_t ping_handler(bb_http_request_t *req) {
    bb_http_resp_set_header(req, "Content-Type", "text/plain");
    bb_http_resp_send(req, "pong\n", 5);
    bb_log_i(TAG, "GET /ping -> pong");
    return BB_OK;
}

void smoke_app_setup(void) {
#ifdef BB_SMOKE_DISPLAY
    bb_display_init();
    bb_display_show_splash("smoke", "v0.0.0");
#endif
    bb_nv_config_init();
    bb_log_i(TAG, "boot");

    uint32_t boot_count = 0;
    bb_nv_get_u32("app", "boot", &boot_count, 0);
    boot_count++;
    bb_nv_set_u32("app", "boot", boot_count);
    bb_log_i(TAG, "boot=%lu", (unsigned long)boot_count);

    bb_wifi_set_hostname("bb-smoke");
    if (bb_wifi_init_sta() != BB_OK) {
        bb_log_e(TAG, "wifi assoc failed");
    }

    bb_http_server_start();
    bb_http_register_route(bb_http_server_get_handle(), BB_HTTP_GET, "/ping", ping_handler);
}

void smoke_app_loop(void) {
    bb_http_server_poll();
}
