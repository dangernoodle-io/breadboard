#pragma once

// Private: shared between platform/espidf and platform/host bb_sensors implementations.
//
// bb_response_assemble_schema(reg, k_sensors_base, k_sensors_suffix) builds the
// complete /api/sensors 200 JSON-Schema.
//
// Root has no fields — only named sections (fan, power, thermal, ...).
// k_sensors_base opens the properties object with no root-level fields.
// k_sensors_suffix closes it and lists required sections.
//
// NOTE: bb_response_assemble_schema detects that base ends with '{' (no content
// yet) so the first section does NOT get a leading comma.

// Fan section schema — PATCH-capable (duty_pct or autofan config).
#ifdef CONFIG_BB_FAN_AUTOFAN
static const char k_sensors_fan_schema[] =
    "{\"type\":\"object\","
    "\"properties\":{"
    "\"present\":{\"type\":\"boolean\"},"
    "\"rpm\":{\"type\":[\"integer\",\"null\"]},"
    "\"duty_pct\":{\"type\":[\"integer\",\"null\"]},"
    "\"die_c\":{\"type\":[\"number\",\"null\"]},"
    "\"board_c\":{\"type\":[\"number\",\"null\"]},"
    "\"autofan\":{\"type\":\"boolean\",\"description\":\"autofan enabled\"},"
    "\"die_target_c\":{\"type\":\"number\",\"description\":\"ASIC die target temperature\"},"
    "\"vr_target_c\":{\"type\":\"number\",\"description\":\"VR target temperature\"},"
    "\"manual_pct\":{\"type\":\"integer\",\"description\":\"manual duty % when autofan disabled\"},"
    "\"min_pct\":{\"type\":\"integer\",\"description\":\"minimum fan duty %\"},"
    "\"die_ema_c\":{\"type\":[\"number\",\"null\"],\"description\":\"filtered ASIC die temperature\"},"
    "\"vr_ema_c\":{\"type\":[\"number\",\"null\"],\"description\":\"filtered VR temperature\"},"
    "\"pid_input_c\":{\"type\":[\"number\",\"null\"],\"description\":\"PID input selected by max(err/target) ratio\"},"
    "\"pid_input_src\":{\"type\":\"string\",\"enum\":[\"die\",\"vr\"],\"description\":\"which sensor is driving PID: die or vr\"}},"
    "\"required\":[\"present\"]}";
#else
static const char k_sensors_fan_schema[] =
    "{\"type\":\"object\","
    "\"properties\":{"
    "\"present\":{\"type\":\"boolean\"},"
    "\"rpm\":{\"type\":[\"integer\",\"null\"]},"
    "\"duty_pct\":{\"type\":[\"integer\",\"null\"]},"
    "\"die_c\":{\"type\":[\"number\",\"null\"]},"
    "\"board_c\":{\"type\":[\"number\",\"null\"]}},"
    "\"required\":[\"present\"]}";
#endif

// Power section schema — read-only.
static const char k_sensors_power_schema[] =
    "{\"type\":\"object\","
    "\"properties\":{"
    "\"present\":{\"type\":\"boolean\"},"
    "\"vout_mv\":{\"type\":[\"integer\",\"null\"]},"
    "\"iout_ma\":{\"type\":[\"integer\",\"null\"]},"
    "\"pout_mw\":{\"type\":[\"integer\",\"null\"]},"
    "\"vin_mv\":{\"type\":[\"integer\",\"null\"]},"
    "\"temp_c\":{\"type\":[\"integer\",\"null\"]}},"
    "\"required\":[\"present\"]}";

// Thermal section schema — read-only.  Each source: {present, c|null}.
static const char k_sensors_thermal_schema[] =
    "{\"type\":\"object\","
    "\"properties\":{"
    "\"soc\":{\"type\":\"object\",\"properties\":{\"present\":{\"type\":\"boolean\"},\"c\":{\"type\":[\"number\",\"null\"]}}},"
    "\"vr\":{\"type\":\"object\",\"properties\":{\"present\":{\"type\":\"boolean\"},\"c\":{\"type\":[\"number\",\"null\"]}}},"
    "\"asic\":{\"type\":\"object\",\"properties\":{\"present\":{\"type\":\"boolean\"},\"c\":{\"type\":[\"number\",\"null\"]}}},"
    "\"board\":{\"type\":\"object\",\"properties\":{\"present\":{\"type\":\"boolean\"},\"c\":{\"type\":[\"number\",\"null\"]}}}},"
    "\"required\":[\"soc\",\"vr\",\"asic\",\"board\"]}";

// PATCH /api/sensors request body schema — fan section only (power/thermal are read-only).
#ifdef CONFIG_BB_FAN_AUTOFAN
static const char k_sensors_patch_request_schema[] =
    "{\"type\":\"object\","
    "\"properties\":{"
    "\"fan\":{\"type\":\"object\","
        "\"properties\":{"
        "\"autofan\":{\"type\":\"boolean\"},"
        "\"die_target_c\":{\"type\":\"number\",\"exclusiveMinimum\":0},"
        "\"vr_target_c\":{\"type\":\"number\",\"exclusiveMinimum\":0},"
        "\"manual_pct\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":100},"
        "\"min_pct\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":100}}}}}";
#else
static const char k_sensors_patch_request_schema[] =
    "{\"type\":\"object\","
    "\"properties\":{"
    "\"fan\":{\"type\":\"object\","
        "\"properties\":{"
        "\"duty_pct\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":100}},"
        "\"required\":[\"duty_pct\"]}}}";
#endif

// Assembled-schema base/suffix: no root-level fields — sections only.
static const char k_sensors_base[] =
    "{\"type\":\"object\","
    "\"properties\":{";

static const char k_sensors_suffix[] =
    "},"
    "\"required\":[\"fan\",\"power\",\"thermal\"]}";
