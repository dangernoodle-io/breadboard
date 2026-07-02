// bb_alert — ESP-IDF only: registry init, topic register, route attach.
// On host this file compiles to an empty translation unit.

#ifdef ESP_PLATFORM

#include "bb_alert.h"

#if BB_ALERT_ENABLE && defined(CONFIG_BB_ALERT_AUTOREGISTER) && CONFIG_BB_ALERT_AUTOREGISTER

#include "bb_log.h"
#include "bb_init.h"
#include "bb_event_routes.h"

static const char *TAG = "bb_alert";

static bb_err_t bb_alert_init(bb_http_handle_t server)
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

// Order 4 — same as bb_ota_check; bb_event_routes_init runs at order 0.
BB_INIT_REGISTER_N(bb_alert, bb_alert_init, 4)

#endif // BB_ALERT_ENABLE && CONFIG_BB_ALERT_AUTOREGISTER

#endif // ESP_PLATFORM
