// bb_alert — ESP-IDF only: registry init, topic register, route attach.
// On host this file compiles to an empty translation unit.

#ifdef ESP_PLATFORM

#include "bb_alert.h"

#if BB_ALERT_ENABLE

#include "bb_log.h"
#include "bb_http_server.h"
#include "bb_event_routes.h"

static const char *TAG = "bb_alert";

bb_err_t bb_alert_init(bb_http_handle_t server)
{
    (void)server;
    bb_err_t err = bb_alert_register();
    if (err != BB_OK) {
        bb_log_e(TAG, "topic register failed: %d", err);
        return err;
    }
    err = bb_event_routes_attach_ex("alert", /*retained=*/false);
    if (err != BB_OK) {
        bb_log_w(TAG, "routes attach failed: %d", err);
    }
    bb_log_i(TAG, "alert topic registered and attached");
    return BB_OK;
}

#endif // BB_ALERT_ENABLE

#endif // ESP_PLATFORM
