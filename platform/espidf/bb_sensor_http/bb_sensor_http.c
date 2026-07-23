// bb_sensor_http — ESP-IDF init glue. Binds fan/power/thermal into bb_data
// (bb_sensor_http_bind_and_register(), components/bb_sensor_http/bb_sensor_http_dispatch.c
// -- portable) then drives bb_http_section_init() to register the real
// GET/PATCH "/api/sensors/*" httpd routes over them (bb_sensor_http PR-2,
// B1-828 epic). No route-handling logic lives in this file -- see
// bb_sensor_http.h for the full per-section contract.
#include "bb_sensor_http.h"
#include "bb_sensor_http_dispatch_priv.h"
#include "bb_http_section.h"
#include "bb_log.h"

static const char *TAG = "bb_sensor_http";

bb_err_t bb_sensor_http_init(bb_http_handle_t server)
{
    if (!server) return BB_ERR_INVALID_ARG;

    bb_err_t err = bb_sensor_http_bind_and_register();
    if (err != BB_OK) {
        bb_log_e(TAG, "failed to bind sensors bb_data bindings: %d", (int)err);
        return err;
    }

    err = bb_http_section_init(server);
    if (err != BB_OK) {
        bb_log_e(TAG, "failed to init http section dispatch: %d", (int)err);
        return err;
    }

    bb_log_i(TAG, "sensors routes registered (/api/sensors/* GET+PATCH)");
    return BB_OK;
}
