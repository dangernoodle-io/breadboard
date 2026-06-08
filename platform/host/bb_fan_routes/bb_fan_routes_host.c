#include "bb_fan_routes.h"
#include "bb_fan.h"
#include "bb_http_extender.h"
#include <stdbool.h>
#include <string.h>

// Host twin of the /api/fan route — provides schema assembly and testing hooks.

#ifdef CONFIG_BB_FAN_AUTOFAN
// Persist callback (shared state — mirrors espidf implementation).
static void (*s_persist_cb)(void *ctx, const bb_fan_autofan_cfg_t *cfg) = NULL;
static void *s_persist_ctx = NULL;

void bb_fan_routes_set_autofan_persist_cb(
    void (*cb)(void *ctx, const bb_fan_autofan_cfg_t *cfg), void *ctx)
{
    s_persist_cb  = cb;
    s_persist_ctx = ctx;
}

// Called by the host POST handler (in test_bb_fan_routes.c) after bb_fan_set_autofan().
void bb_fan_routes_invoke_persist_cb(const bb_fan_autofan_cfg_t *cfg)
{
    if (s_persist_cb) {
        s_persist_cb(s_persist_ctx, cfg);
    }
}
#endif /* CONFIG_BB_FAN_AUTOFAN */

#ifdef CONFIG_BB_FAN_AUTOFAN
static const char k_fan_schema_base[] =
    "{\"type\":\"object\","
    "\"properties\":{"
    "\"present\":{\"type\":\"boolean\"},"
    "\"rpm\":{\"type\":[\"integer\",\"null\"]},"
    "\"duty_pct\":{\"type\":[\"integer\",\"null\"]},"
    "\"autofan\":{\"type\":\"boolean\",\"description\":\"autofan enabled\"},"
    "\"die_target_c\":{\"type\":\"number\",\"description\":\"ASIC die target temperature\"},"
    "\"vr_target_c\":{\"type\":\"number\",\"description\":\"VR target temperature\"},"
    "\"manual_pct\":{\"type\":\"integer\",\"description\":\"manual duty % when autofan disabled\"},"
    "\"min_pct\":{\"type\":\"integer\",\"description\":\"minimum fan duty %\"},"
    "\"die_ema_c\":{\"type\":[\"number\",\"null\"],\"description\":\"filtered ASIC die temperature\"},"
    "\"vr_ema_c\":{\"type\":[\"number\",\"null\"],\"description\":\"filtered VR temperature\"},"
    "\"pid_input_c\":{\"type\":[\"number\",\"null\"],\"description\":\"PID input selected by max(err/target) ratio\"},"
    "\"pid_input_src\":{\"type\":\"string\",\"enum\":[\"die\",\"vr\"],\"description\":\"which sensor is driving PID: die or vr\"}";
#else
static const char k_fan_schema_base[] =
    "{\"type\":\"object\","
    "\"properties\":{"
    "\"present\":{\"type\":\"boolean\"},"
    "\"rpm\":{\"type\":[\"integer\",\"null\"]},"
    "\"duty_pct\":{\"type\":[\"integer\",\"null\"]}";
#endif

static const char k_fan_schema_suffix[] =
    "},"
    "\"required\":[\"present\"]}";

#ifdef BB_FAN_ROUTES_TESTING

const char *bb_fan_routes_get_assembled_schema(void)
{
    const char *cached = bb_http_extender_get_assembled_schema("fan");
    if (cached) return cached;
    return bb_http_route_assemble_schema("fan", k_fan_schema_base, k_fan_schema_suffix);
}

void bb_fan_routes_reset_for_test(void)
{
    bb_fan_test_reset();
#ifdef CONFIG_BB_FAN_AUTOFAN
    s_persist_cb  = NULL;
    s_persist_ctx = NULL;
#endif
    // extender reset is handled globally by bb_info_reset_for_test → bb_http_extender_reset_for_test
}

#endif /* BB_FAN_ROUTES_TESTING */
