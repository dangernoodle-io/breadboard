#include "bb_temp.h"
#include "bb_health.h"
#include "bb_json.h"
#include "bb_system.h"

#if defined(CONFIG_BB_TEMP_AUTOREGISTER) && CONFIG_BB_TEMP_AUTOREGISTER
#include "bb_init.h"
#endif

/* JSON-Schema value for the "temp" section contributed to the /api/health 200 schema. */
static const char k_temp_schema[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"present\":{\"type\":\"boolean\"},"
    "\"soc_c\":{\"type\":\"number\"}}}";

void bb_temp_emit_section(bb_json_t obj)
{
    float c = 0.0f;
    if (bb_temp_read_soc(&c)) {
        bb_json_obj_set_bool(obj, "present", true);
        /* Round to 1 decimal: multiply, round, divide. */
        double rounded = (double)((int)(c * 10.0f + 0.5f)) / 10.0;
        bb_json_obj_set_number(obj, "soc_c", rounded);
    } else {
        bb_json_obj_set_bool(obj, "present", false);
    }
}

static void temp_section_get(bb_json_t section, void *ctx)
{
    (void)ctx;
    bb_temp_emit_section(section);
}

bool bb_temp_read_soc(float *out_celsius)
{
    if (!out_celsius) return false;
    return bb_system_read_temp_celsius(out_celsius) == BB_OK;
}

void bb_temp_register_info(void)
{
    bb_health_register_section("temp", temp_section_get, NULL, k_temp_schema);
}

#if defined(CONFIG_BB_TEMP_AUTOREGISTER) && CONFIG_BB_TEMP_AUTOREGISTER

/* order 1: after bb_health PRE_HTTP init; section table is open until
 * bb_info_freeze (order 20). Mirrors the sequencing of the manual call. */
static bb_err_t bb_temp_autoregister_init(bb_http_handle_t server)
{
    (void)server;
    bb_temp_register_info();
    return BB_OK;
}

BB_INIT_REGISTER_N(bb_temp, bb_temp_autoregister_init, 1);

#endif /* CONFIG_BB_TEMP_AUTOREGISTER */
