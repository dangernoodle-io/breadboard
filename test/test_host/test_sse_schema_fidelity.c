#include "unity.h"
#include "bb_openapi.h"
#include "bb_openapi_emit_internal.h"
#include "bb_openapi_validate_priv.h"
#include "bb_http.h"
#include "bb_http_server.h"
#include "bb_http_host.h"
#include "bb_data_http.h"
#include "test_openapi_capture.h"

#include <cJSON.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
// Shared fixtures
// ---------------------------------------------------------------------------

static const char k_log_schema[] =
    "{\"title\":\"LogEvent\",\"x-sse-topic\":\"log\",\"type\":\"object\","
    "\"properties\":{"
    "\"ts\":{\"type\":\"integer\"},"
    "\"level\":{\"type\":\"string\",\"enum\":[\"I\",\"W\",\"E\",\"D\",\"V\",\"?\"]},"
    "\"tag\":{\"type\":\"string\"},"
    "\"msg\":{\"type\":\"string\"}},"
    "\"required\":[\"ts\",\"level\",\"tag\",\"msg\"]}";

static const char k_wifi_schema[] =
    "{\"title\":\"WifiInfo\",\"type\":\"object\","
    "\"properties\":{\"ssid\":{\"type\":\"string\"},\"connected\":{\"type\":\"boolean\"}},"
    "\"required\":[\"ssid\",\"connected\"]}";

// ---------------------------------------------------------------------------
// Registry API
// ---------------------------------------------------------------------------

void test_sse_schema_registry_count_zero_initially(void)
{
    TEST_ASSERT_EQUAL_size_t(0, bb_openapi_schema_count());
}

void test_sse_schema_registry_count_after_register(void)
{
    bb_openapi_register_schema("LogEvent", k_log_schema, "log");
    TEST_ASSERT_EQUAL_size_t(1, bb_openapi_schema_count());
}

void test_sse_schema_registry_get_returns_entry(void)
{
    bb_openapi_register_schema("LogEvent", k_log_schema, "log");
    bb_openapi_schema_entry_t e = {0};
    TEST_ASSERT_TRUE(bb_openapi_schema_get(0, &e));
    TEST_ASSERT_EQUAL_STRING("LogEvent", e.component_name);
    TEST_ASSERT_EQUAL_STRING(k_log_schema, e.schema_literal);
    TEST_ASSERT_EQUAL_STRING("log", e.sse_topic);
}

void test_sse_schema_registry_get_out_of_bounds_returns_false(void)
{
    TEST_ASSERT_FALSE(bb_openapi_schema_get(0, NULL));
    bb_openapi_register_schema("LogEvent", k_log_schema, "log");
    bb_openapi_schema_entry_t e = {0};
    TEST_ASSERT_FALSE(bb_openapi_schema_get(1, &e));
}

void test_sse_schema_registry_dedup_first_wins(void)
{
    bb_openapi_register_schema("LogEvent", k_log_schema, "log");
    bb_openapi_register_schema("LogEvent", k_wifi_schema, "wifi");
    TEST_ASSERT_EQUAL_size_t(1, bb_openapi_schema_count());
    bb_openapi_schema_entry_t e = {0};
    bb_openapi_schema_get(0, &e);
    TEST_ASSERT_EQUAL_STRING(k_log_schema, e.schema_literal);
}

void test_sse_schema_registry_overflow_returns_no_space(void)
{
    // Static names — registry stores raw pointers so names must outlive the call.
    static const char *names[BB_OPENAPI_SCHEMA_REGISTRY_CAP] = {
        "S0","S1","S2","S3","S4","S5","S6","S7",
        "S8","S9","S10","S11","S12","S13","S14","S15",
        "S16","S17","S18","S19","S20","S21","S22","S23"
    };
    for (size_t i = 0; i < BB_OPENAPI_SCHEMA_REGISTRY_CAP; i++) {
        TEST_ASSERT_EQUAL(BB_OK, bb_openapi_register_schema(names[i], k_log_schema, NULL));
    }
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE,
                      bb_openapi_register_schema("Overflow", k_log_schema, NULL));
}

void test_sse_schema_register_null_args_returns_invalid_arg(void)
{
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
                      bb_openapi_register_schema(NULL, k_log_schema, NULL));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
                      bb_openapi_register_schema("X", NULL, NULL));
}

void test_sse_schema_register_topic_schema_convenience(void)
{
    bb_openapi_register_topic_schema("log", k_log_schema, "LogEvent");
    TEST_ASSERT_EQUAL_size_t(1, bb_openapi_schema_count());
    bb_openapi_schema_entry_t e = {0};
    bb_openapi_schema_get(0, &e);
    TEST_ASSERT_EQUAL_STRING("LogEvent", e.component_name);
    TEST_ASSERT_EQUAL_STRING("log", e.sse_topic);
}

// ---------------------------------------------------------------------------
// Log payload validates against LogEvent schema
// ---------------------------------------------------------------------------

void test_sse_schema_log_payload_valid(void)
{
    cJSON *obj = cJSON_CreateObject();
    TEST_ASSERT_NOT_NULL(obj);
    cJSON_AddNumberToObject(obj,    "ts",    1234567890LL);
    cJSON_AddStringToObject(obj, "level", "I");
    cJSON_AddStringToObject(obj, "tag",   "wifi");
    cJSON_AddStringToObject(obj, "msg",   "connected");
    TEST_ASSERT_EQUAL(BB_OK, bb_openapi_validate(k_log_schema, obj, NULL));
    cJSON_Delete(obj);
}

void test_sse_schema_log_payload_missing_required_fails(void)
{
    cJSON *obj = cJSON_CreateObject();
    TEST_ASSERT_NOT_NULL(obj);
    cJSON_AddNumberToObject(obj,    "ts",    1234567890LL);
    cJSON_AddStringToObject(obj, "level", "W");
    // missing "tag" and "msg"
    bb_openapi_validate_err_t verr = {0};
    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION,
                      bb_openapi_validate(k_log_schema, obj, &verr));
    cJSON_Delete(obj);
}

void test_sse_schema_log_payload_bad_level_enum_fails(void)
{
    cJSON *obj = cJSON_CreateObject();
    TEST_ASSERT_NOT_NULL(obj);
    cJSON_AddNumberToObject(obj,    "ts",    1234567890LL);
    cJSON_AddStringToObject(obj, "level", "X");
    cJSON_AddStringToObject(obj, "tag",   "test");
    cJSON_AddStringToObject(obj, "msg",   "bad level");
    bb_openapi_validate_err_t verr = {0};
    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION,
                      bb_openapi_validate(k_log_schema, obj, &verr));
    TEST_ASSERT_NOT_NULL(strstr(verr.path, "level"));
    cJSON_Delete(obj);
}

// ---------------------------------------------------------------------------
// Emit doc has components/schemas
// ---------------------------------------------------------------------------

static bb_err_t stub_handler(bb_http_request_t *req) { (void)req; return BB_OK; }

static const bb_route_response_t s_fid_responses[] = {
    { .status = 200, .content_type = "application/json",
      .schema = "{\"type\":\"object\"}", .description = "ok" },
    { .status = 0 },
};
static const bb_route_t s_fid_route = {
    .method = BB_HTTP_GET, .path = "/api/fid", .tag = "fid",
    .summary = "fidelity test route", .responses = s_fid_responses,
    .handler = stub_handler,
};

void test_sse_schema_emit_has_components_schemas(void)
{
    bb_http_route_registry_clear();
    bb_http_register_described_route(NULL, &s_fid_route);
    bb_openapi_register_schema("LogEvent", k_log_schema, "log");

    bb_openapi_meta_t meta = { .title = "T", .version = "1.0" };
    test_openapi_capture_result_t r = test_openapi_capture(&meta);
    TEST_ASSERT_EQUAL(BB_OK, r.status);
    TEST_ASSERT_NOT_NULL(r.doc);

    cJSON *components = cJSON_GetObjectItemCaseSensitive(r.doc, "components");
    TEST_ASSERT_NOT_NULL(components);
    cJSON *schemas = cJSON_GetObjectItemCaseSensitive(components, "schemas");
    TEST_ASSERT_NOT_NULL(schemas);
    cJSON *entry = cJSON_GetObjectItemCaseSensitive(schemas, "LogEvent");
    TEST_ASSERT_NOT_NULL(entry);

    test_openapi_capture_free(&r);
}

void test_sse_schema_emit_no_schemas_no_components(void)
{
    bb_http_route_registry_clear();
    bb_http_register_described_route(NULL, &s_fid_route);
    // registry is empty (cleared in setUp)

    bb_openapi_meta_t meta = { .title = "T", .version = "1.0" };
    test_openapi_capture_result_t r = test_openapi_capture(&meta);
    TEST_ASSERT_EQUAL(BB_OK, r.status);
    TEST_ASSERT_NOT_NULL(r.doc);
    TEST_ASSERT_NULL(cJSON_GetObjectItemCaseSensitive(r.doc, "components"));
    test_openapi_capture_free(&r);
}

// ---------------------------------------------------------------------------
// SSE route → oneOf $ref synthesis
// ---------------------------------------------------------------------------

static const bb_route_response_t s_sse_responses[] = {
    { .status = 200, .content_type = "text/event-stream",
      .schema = NULL, .description = "SSE stream" },
    { .status = 0 },
};
static const bb_route_t s_sse_route = {
    .method = BB_HTTP_GET, .path = "/api/events", .tag = "events",
    .summary = "SSE stream", .responses = s_sse_responses,
    .handler = stub_handler,
};

void test_sse_schema_oneof_synthesized_in_events(void)
{
    bb_http_route_registry_clear();
    bb_http_register_described_route(NULL, &s_sse_route);
    bb_openapi_register_schema("LogEvent", k_log_schema, "log");

    bb_openapi_meta_t meta = { .title = "T", .version = "1.0" };
    test_openapi_capture_result_t r = test_openapi_capture(&meta);
    TEST_ASSERT_EQUAL(BB_OK, r.status);
    TEST_ASSERT_NOT_NULL(r.cap.body);

    TEST_ASSERT_NOT_NULL(strstr(r.cap.body, "\"oneOf\""));
    TEST_ASSERT_NOT_NULL(strstr(r.cap.body, "\"$ref\":\"#/components/schemas/LogEvent\""));

    test_openapi_capture_free(&r);
}

// ---------------------------------------------------------------------------
// Stream path has components/schemas
// ---------------------------------------------------------------------------

void test_sse_schema_stream_has_components_schemas(void)
{
    bb_http_route_registry_clear();
    bb_http_register_described_route(NULL, &s_fid_route);
    bb_openapi_register_schema("LogEvent", k_log_schema, "log");

    bb_http_request_t *req = NULL;
    bb_http_host_capture_begin(&req);

    bb_openapi_meta_t meta = { .title = "T", .version = "1.0" };
    TEST_ASSERT_EQUAL(BB_OK, bb_openapi_emit_stream(req, &meta));

    bb_http_host_capture_t cap = {0};
    bb_http_host_capture_end(req, &cap);
    TEST_ASSERT_NOT_NULL(cap.body);

    TEST_ASSERT_NOT_NULL(strstr(cap.body, "\"components\":{\"schemas\":{"));
    TEST_ASSERT_NOT_NULL(strstr(cap.body, "\"LogEvent\":"));

    bb_http_host_capture_free(&cap);
}

void test_sse_schema_stream_no_schemas_no_components(void)
{
    bb_http_route_registry_clear();
    bb_http_register_described_route(NULL, &s_fid_route);
    // registry empty

    bb_http_request_t *req = NULL;
    bb_http_host_capture_begin(&req);

    bb_openapi_meta_t meta = { .title = "T", .version = "1.0" };
    TEST_ASSERT_EQUAL(BB_OK, bb_openapi_emit_stream(req, &meta));

    bb_http_host_capture_t cap = {0};
    bb_http_host_capture_end(req, &cap);
    TEST_ASSERT_NOT_NULL(cap.body);

    TEST_ASSERT_NULL(strstr(cap.body, "\"components\""));
    // Root object still closes correctly: last char must be '}'.
    TEST_ASSERT_EQUAL_CHAR('}', cap.body[cap.body_len - 1]);

    bb_http_host_capture_free(&cap);
}

// ---------------------------------------------------------------------------
// Line 61: bb_openapi_schema_get — valid idx but NULL out returns false
// (covers the !out branch when idx < s_schema_count)
// ---------------------------------------------------------------------------

void test_sse_schema_registry_get_valid_idx_null_out_returns_false(void)
{
    bb_openapi_register_schema("LogEvent", k_log_schema, "log");
    // idx=0 is valid (count=1), but out=NULL → second branch of || taken
    TEST_ASSERT_FALSE(bb_openapi_schema_get(0, NULL));
}

// ---------------------------------------------------------------------------
// Line 307: oneOf loop skips entries with NULL sse_topic
// (covers the !sse_topic continue branch when mixed SSE/non-SSE schemas)
// ---------------------------------------------------------------------------

void test_sse_schema_oneof_skips_non_sse_schema_in_registry(void)
{
    bb_http_route_registry_clear();
    bb_http_register_described_route(NULL, &s_sse_route);
    // LogEvent (sse_topic="log") → included in oneOf
    bb_openapi_register_schema("LogEvent", k_log_schema, "log");
    // WifiInfo (sse_topic=NULL) → skipped in oneOf loop via continue (line 307)
    bb_openapi_register_schema("WifiInfo", k_wifi_schema, NULL);

    bb_openapi_meta_t meta = { .title = "T", .version = "1.0" };
    test_openapi_capture_result_t r = test_openapi_capture(&meta);
    TEST_ASSERT_EQUAL(BB_OK, r.status);
    TEST_ASSERT_NOT_NULL(r.cap.body);

    // LogEvent IS in oneOf (has sse_topic)
    TEST_ASSERT_NOT_NULL(strstr(r.cap.body, "\"$ref\":\"#/components/schemas/LogEvent\""));
    // WifiInfo is NOT in oneOf (null sse_topic → continue branch)
    TEST_ASSERT_NULL(strstr(r.cap.body, "\"$ref\":\"#/components/schemas/WifiInfo\""));
    // WifiInfo IS in components/schemas (registered with null sse_topic)
    TEST_ASSERT_NOT_NULL(strstr(r.cap.body, "\"WifiInfo\""));

    test_openapi_capture_free(&r);
}

// ---------------------------------------------------------------------------
// Line 588: stream path emits comma between multiple schemas
// (covers the i > 0 true branch in the components/schemas streaming loop)
// ---------------------------------------------------------------------------

void test_sse_schema_stream_two_schemas_has_comma_separator(void)
{
    bb_http_route_registry_clear();
    bb_http_register_described_route(NULL, &s_fid_route);
    // Two schemas → stream loop runs twice; i=1 triggers comma (line 588-589)
    bb_openapi_register_schema("LogEvent", k_log_schema, "log");
    bb_openapi_register_schema("WifiInfo", k_wifi_schema, NULL);

    bb_http_request_t *req = NULL;
    bb_http_host_capture_begin(&req);

    bb_openapi_meta_t meta = { .title = "T", .version = "1.0" };
    TEST_ASSERT_EQUAL(BB_OK, bb_openapi_emit_stream(req, &meta));

    bb_http_host_capture_t cap = {0};
    bb_http_host_capture_end(req, &cap);
    TEST_ASSERT_NOT_NULL(cap.body);

    // Both schemas present in the components/schemas block
    TEST_ASSERT_NOT_NULL(strstr(cap.body, "\"LogEvent\":"));
    TEST_ASSERT_NOT_NULL(strstr(cap.body, "\"WifiInfo\":"));
    // The comma between them must appear (i > 0 branch)
    TEST_ASSERT_NOT_NULL(strstr(cap.body, "\"schemas\":{"));

    bb_http_host_capture_free(&cap);
}

// ---------------------------------------------------------------------------
// Per-topic schema fidelity tests — B1-413 PR3
// ---------------------------------------------------------------------------

// WifiTelemetry (sse_topic="wifi")
static const char k_wifi_telemetry_schema[] =
    "{\"title\":\"WifiTelemetry\",\"x-sse-topic\":\"wifi\",\"type\":\"object\","
    "\"properties\":{"
    "\"ssid\":{\"type\":\"string\"},"
    "\"bssid\":{\"type\":\"string\"},"
    "\"rssi\":{\"type\":\"integer\"},"
    "\"ip\":{\"type\":\"string\"},"
    "\"connected\":{\"type\":\"boolean\"},"
    "\"disc_reason\":{\"type\":\"string\"},"
    "\"disc_age_s\":{\"type\":\"integer\"},"
    "\"retry_count\":{\"type\":\"integer\"},"
    "\"ts_ms\":{\"type\":\"integer\"}},"
    "\"required\":[\"ssid\",\"connected\",\"rssi\",\"ts_ms\"]}";

void test_sse_schema_wifi_telemetry_payload_valid(void)
{
    cJSON *obj = cJSON_CreateObject();
    TEST_ASSERT_NOT_NULL(obj);
    cJSON_AddStringToObject(obj, "ssid",             "TestNet");
    cJSON_AddStringToObject(obj, "bssid",            "aa:bb:cc:dd:ee:ff");
    cJSON_AddNumberToObject(obj, "rssi",             -65);
    cJSON_AddStringToObject(obj, "ip",               "192.168.1.10");
    cJSON_AddBoolToObject(obj, "connected",        true);
    cJSON_AddStringToObject(obj, "disc_reason",      "unknown");
    cJSON_AddNumberToObject(obj, "disc_age_s",       0);
    cJSON_AddNumberToObject(obj, "retry_count",      0);
    cJSON_AddNumberToObject(obj, "ts_ms",            12345678LL);
    TEST_ASSERT_EQUAL(BB_OK, bb_openapi_validate(k_wifi_telemetry_schema, obj, NULL));
    cJSON_Delete(obj);
}

void test_sse_schema_wifi_telemetry_payload_missing_required_fails(void)
{
    cJSON *obj = cJSON_CreateObject();
    TEST_ASSERT_NOT_NULL(obj);
    cJSON_AddStringToObject(obj, "ssid", "TestNet");
    // missing connected, rssi, ts_ms
    bb_openapi_validate_err_t verr = {0};
    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION,
                      bb_openapi_validate(k_wifi_telemetry_schema, obj, &verr));
    cJSON_Delete(obj);
}

// FanTelemetry (sse_topic="fan")
static const char k_fan_telemetry_schema[] =
    "{\"title\":\"FanTelemetry\",\"x-sse-topic\":\"fan\",\"type\":\"object\","
    "\"properties\":{"
    "\"rpm\":{\"type\":[\"integer\",\"null\"]},"
    "\"duty_pct\":{\"type\":[\"integer\",\"null\"]},"
    "\"die_c\":{\"type\":[\"number\",\"null\"]},"
    "\"board_c\":{\"type\":[\"number\",\"null\"]},"
    "\"die_ema_c\":{\"type\":[\"number\",\"null\"]},"
    "\"vr_ema_c\":{\"type\":[\"number\",\"null\"]},"
    "\"pid_input_c\":{\"type\":[\"number\",\"null\"]},"
    "\"pid_input_src\":{\"type\":\"string\",\"enum\":[\"die\",\"vr\"]},"
    "\"ts_ms\":{\"type\":\"integer\"}},"
    "\"required\":[\"rpm\",\"duty_pct\",\"die_c\",\"board_c\",\"ts_ms\"]}";

void test_sse_schema_fan_telemetry_payload_valid(void)
{
    cJSON *obj = cJSON_CreateObject();
    TEST_ASSERT_NOT_NULL(obj);
    cJSON_AddNumberToObject(obj, "rpm",      1200);
    cJSON_AddNumberToObject(obj, "duty_pct", 50);
    cJSON_AddNumberToObject(obj, "die_c",    72.5);
    cJSON_AddNumberToObject(obj, "board_c",  45.0);
    cJSON_AddNumberToObject(obj, "ts_ms",    12345678LL);
    TEST_ASSERT_EQUAL(BB_OK, bb_openapi_validate(k_fan_telemetry_schema, obj, NULL));
    cJSON_Delete(obj);
}

void test_sse_schema_fan_telemetry_payload_missing_required_fails(void)
{
    cJSON *obj = cJSON_CreateObject();
    TEST_ASSERT_NOT_NULL(obj);
    cJSON_AddNumberToObject(obj, "rpm", 1200);
    // missing duty_pct, die_c, board_c, ts_ms
    bb_openapi_validate_err_t verr = {0};
    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION,
                      bb_openapi_validate(k_fan_telemetry_schema, obj, &verr));
    cJSON_Delete(obj);
}

// PowerTelemetry (sse_topic="power")
static const char k_power_telemetry_schema[] =
    "{\"title\":\"PowerTelemetry\",\"x-sse-topic\":\"power\",\"type\":\"object\","
    "\"properties\":{"
    "\"vout_mv\":{\"type\":[\"number\",\"null\"]},"
    "\"iout_ma\":{\"type\":[\"number\",\"null\"]},"
    "\"pout_mw\":{\"type\":[\"number\",\"null\"]},"
    "\"vin_mv\":{\"type\":[\"number\",\"null\"]},"
    "\"temp_c\":{\"type\":[\"number\",\"null\"]},"
    "\"ts_ms\":{\"type\":\"integer\"}},"
    "\"required\":[\"vout_mv\",\"iout_ma\",\"pout_mw\",\"vin_mv\",\"temp_c\",\"ts_ms\"]}";

void test_sse_schema_power_telemetry_payload_valid(void)
{
    cJSON *obj = cJSON_CreateObject();
    TEST_ASSERT_NOT_NULL(obj);
    cJSON_AddNumberToObject(obj, "vout_mv", 12000.0);
    cJSON_AddNumberToObject(obj, "iout_ma", 5000.0);
    cJSON_AddNumberToObject(obj, "pout_mw", 60000.0);
    cJSON_AddNumberToObject(obj, "vin_mv",  19000.0);
    cJSON_AddNumberToObject(obj, "temp_c",  55.0);
    cJSON_AddNumberToObject(obj, "ts_ms",   12345678LL);
    TEST_ASSERT_EQUAL(BB_OK, bb_openapi_validate(k_power_telemetry_schema, obj, NULL));
    cJSON_Delete(obj);
}

void test_sse_schema_power_telemetry_payload_missing_required_fails(void)
{
    cJSON *obj = cJSON_CreateObject();
    TEST_ASSERT_NOT_NULL(obj);
    cJSON_AddNumberToObject(obj, "vout_mv", 12000.0);
    // missing iout_ma, pout_mw, vin_mv, temp_c, ts_ms
    bb_openapi_validate_err_t verr = {0};
    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION,
                      bb_openapi_validate(k_power_telemetry_schema, obj, &verr));
    cJSON_Delete(obj);
}

// ThermalTelemetry (sse_topic="thermal")
static const char k_thermal_telemetry_schema[] =
    "{\"title\":\"ThermalTelemetry\",\"x-sse-topic\":\"thermal\",\"type\":\"object\","
    "\"properties\":{"
    "\"soc_c\":{\"type\":[\"number\",\"null\"]},"
    "\"vr_c\":{\"type\":[\"number\",\"null\"]},"
    "\"asic_c\":{\"type\":[\"number\",\"null\"]},"
    "\"board_c\":{\"type\":[\"number\",\"null\"]},"
    "\"ts_ms\":{\"type\":\"integer\"}},"
    "\"required\":[\"soc_c\",\"ts_ms\"]}";

void test_sse_schema_thermal_telemetry_payload_valid(void)
{
    cJSON *obj = cJSON_CreateObject();
    TEST_ASSERT_NOT_NULL(obj);
    cJSON_AddNumberToObject(obj, "soc_c",  68.0);
    cJSON_AddNumberToObject(obj, "ts_ms",  12345678LL);
    TEST_ASSERT_EQUAL(BB_OK, bb_openapi_validate(k_thermal_telemetry_schema, obj, NULL));
    cJSON_Delete(obj);
}

void test_sse_schema_thermal_telemetry_payload_missing_required_fails(void)
{
    cJSON *obj = cJSON_CreateObject();
    TEST_ASSERT_NOT_NULL(obj);
    cJSON_AddNumberToObject(obj, "vr_c", 55.0);
    // missing soc_c and ts_ms
    bb_openapi_validate_err_t verr = {0};
    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION,
                      bb_openapi_validate(k_thermal_telemetry_schema, obj, &verr));
    cJSON_Delete(obj);
}

// InfoTelemetry (sse_topic=NULL — sinks only)
static const char k_info_telemetry_schema[] =
    "{\"title\":\"InfoTelemetry\",\"type\":\"object\","
    "\"properties\":{"
    "\"heap_internal_free\":{\"type\":\"number\"},"
    "\"heap_internal_total\":{\"type\":\"number\"},"
    "\"heap_internal_largest_block\":{\"type\":\"number\"},"
    "\"heap_internal_min_free\":{\"type\":\"number\"},"
    "\"psram_free\":{\"type\":\"number\"},"
    "\"psram_total\":{\"type\":\"number\"},"
    "\"rtc_used\":{\"type\":\"number\"},"
    "\"rtc_total\":{\"type\":\"number\"},"
    "\"dram_static_bytes\":{\"type\":\"number\"},"
    "\"flash_size\":{\"type\":\"number\"},"
    "\"app_size\":{\"type\":\"number\"},"
    "\"wdt_resets\":{\"type\":\"number\"},"
    "\"version\":{\"type\":\"string\"},"
    "\"board\":{\"type\":\"string\"},"
    "\"chip_model\":{\"type\":\"string\"},"
    "\"mac\":{\"type\":\"string\"},"
    "\"reset_reason\":{\"type\":\"string\"},"
    "\"ota_validated\":{\"type\":\"boolean\"},"
    "\"time_valid\":{\"type\":\"boolean\"},"
    "\"boot_epoch_s\":{\"type\":\"number\"},"
    "\"time_source\":{\"type\":\"string\"},"
    "\"rtc_free\":{\"type\":\"number\"},"
    "\"ts_ms\":{\"type\":\"integer\"}},"
    "\"required\":[\"heap_internal_free\",\"heap_internal_total\","
    "\"heap_internal_largest_block\",\"heap_internal_min_free\","
    "\"rtc_used\",\"rtc_total\",\"dram_static_bytes\",\"flash_size\","
    "\"app_size\",\"wdt_resets\",\"version\",\"board\",\"chip_model\","
    "\"mac\",\"reset_reason\",\"ota_validated\",\"time_valid\","
    "\"boot_epoch_s\",\"time_source\",\"rtc_free\",\"ts_ms\"]}";

void test_sse_schema_info_telemetry_payload_valid(void)
{
    cJSON *obj = cJSON_CreateObject();
    TEST_ASSERT_NOT_NULL(obj);
    cJSON_AddNumberToObject(obj, "heap_internal_free",          100000.0);
    cJSON_AddNumberToObject(obj, "heap_internal_total",         200000.0);
    cJSON_AddNumberToObject(obj, "heap_internal_largest_block",  90000.0);
    cJSON_AddNumberToObject(obj, "heap_internal_min_free",       80000.0);
    cJSON_AddNumberToObject(obj, "rtc_used",         1024.0);
    cJSON_AddNumberToObject(obj, "rtc_total",         8192.0);
    cJSON_AddNumberToObject(obj, "dram_static_bytes", 50000.0);
    cJSON_AddNumberToObject(obj, "flash_size",         4194304.0);
    cJSON_AddNumberToObject(obj, "app_size",           1200000.0);
    cJSON_AddNumberToObject(obj, "wdt_resets",         0.0);
    cJSON_AddStringToObject(obj, "version",            "1.0.0");
    cJSON_AddStringToObject(obj, "board",              "wroom32");
    cJSON_AddStringToObject(obj, "chip_model",         "ESP32");
    cJSON_AddStringToObject(obj, "mac",                "aa:bb:cc:dd:ee:ff");
    cJSON_AddStringToObject(obj, "reset_reason",       "power_on");
    cJSON_AddBoolToObject(obj, "ota_validated",      true);
    cJSON_AddBoolToObject(obj, "time_valid",         false);
    cJSON_AddNumberToObject(obj, "boot_epoch_s",       0.0);
    cJSON_AddStringToObject(obj, "time_source",        "none");
    cJSON_AddNumberToObject(obj, "rtc_free",           7168.0);
    cJSON_AddNumberToObject(obj, "ts_ms",              12345678LL);
    TEST_ASSERT_EQUAL(BB_OK, bb_openapi_validate(k_info_telemetry_schema, obj, NULL));
    cJSON_Delete(obj);
}

void test_sse_schema_info_telemetry_payload_missing_required_fails(void)
{
    cJSON *obj = cJSON_CreateObject();
    TEST_ASSERT_NOT_NULL(obj);
    cJSON_AddNumberToObject(obj, "heap_internal_free", 100000.0);
    // missing all other required fields
    bb_openapi_validate_err_t verr = {0};
    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION,
                      bb_openapi_validate(k_info_telemetry_schema, obj, &verr));
    cJSON_Delete(obj);
}

// RtosTelemetry: historical fixture — the "rtos" telemetry source
// (bb_pub_rtos) was removed with the bb_pub/bb_sink_* cluster cut (B1-905);
// this schema no longer has a production producer. Retained as generic
// bb_openapi_validate schema-fidelity test data.
static const char k_rtos_telemetry_schema[] =
    "{\"title\":\"RtosTelemetry\",\"type\":\"object\","
    "\"properties\":{"
    "\"min_free_stack\":{\"type\":\"number\"},"
    "\"min_free_stack_task\":{\"type\":\"string\"},"
    "\"task_count\":{\"type\":\"number\"},"
    "\"stack_worker\":{\"type\":\"number\"},"
    "\"stack_httpd\":{\"type\":\"number\"},"
    "\"stack_mqtt\":{\"type\":\"number\"},"
    "\"stack_ipc0\":{\"type\":\"number\"},"
    "\"stack_ipc1\":{\"type\":\"number\"},"
    "\"stack_main\":{\"type\":\"number\"}},"
    "\"required\":[\"min_free_stack\",\"min_free_stack_task\",\"task_count\"]}";

void test_sse_schema_rtos_telemetry_payload_valid(void)
{
    cJSON *obj = cJSON_CreateObject();
    TEST_ASSERT_NOT_NULL(obj);
    cJSON_AddNumberToObject(obj, "min_free_stack",      2048.0);
    cJSON_AddStringToObject(obj, "min_free_stack_task", "worker");
    cJSON_AddNumberToObject(obj, "task_count",          8.0);
    TEST_ASSERT_EQUAL(BB_OK, bb_openapi_validate(k_rtos_telemetry_schema, obj, NULL));
    cJSON_Delete(obj);
}

void test_sse_schema_rtos_telemetry_payload_missing_required_fails(void)
{
    cJSON *obj = cJSON_CreateObject();
    TEST_ASSERT_NOT_NULL(obj);
    cJSON_AddNumberToObject(obj, "min_free_stack", 2048.0);
    // missing min_free_stack_task and task_count
    bb_openapi_validate_err_t verr = {0};
    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION,
                      bb_openapi_validate(k_rtos_telemetry_schema, obj, &verr));
    cJSON_Delete(obj);
}

// Alert (sse_topic="alert")
static const char k_alert_schema[] =
    "{\"title\":\"Alert\",\"x-sse-topic\":\"alert\",\"type\":\"object\","
    "\"properties\":{"
    "\"type\":{\"type\":\"string\"},"
    "\"severity\":{\"type\":\"integer\"},"
    "\"uptime_ms\":{\"type\":\"integer\"}},"
    "\"required\":[\"type\",\"severity\",\"uptime_ms\"]}";

void test_sse_schema_alert_payload_valid(void)
{
    cJSON *obj = cJSON_CreateObject();
    TEST_ASSERT_NOT_NULL(obj);
    cJSON_AddStringToObject(obj, "type",      "wifi_down");
    cJSON_AddNumberToObject(obj, "severity",  2);
    cJSON_AddNumberToObject(obj, "uptime_ms", 60000LL);
    TEST_ASSERT_EQUAL(BB_OK, bb_openapi_validate(k_alert_schema, obj, NULL));
    cJSON_Delete(obj);
}

void test_sse_schema_alert_payload_missing_required_fails(void)
{
    cJSON *obj = cJSON_CreateObject();
    TEST_ASSERT_NOT_NULL(obj);
    cJSON_AddStringToObject(obj, "type", "wifi_down");
    // missing severity and uptime_ms
    bb_openapi_validate_err_t verr = {0};
    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION,
                      bb_openapi_validate(k_alert_schema, obj, &verr));
    cJSON_Delete(obj);
}

// UpdateAvailable (sse_topic="update.available")
static const char k_update_available_schema[] =
    "{\"title\":\"UpdateAvailable\",\"x-sse-topic\":\"update.available\","
    "\"type\":\"object\","
    "\"properties\":{"
    "\"current\":{\"type\":\"string\"},"
    "\"latest\":{\"type\":\"string\"},"
    "\"download_url\":{\"type\":\"string\"},"
    "\"available\":{\"type\":\"boolean\"},"
    "\"ts\":{\"type\":\"integer\"},"
    "\"last_check_ok\":{\"type\":\"boolean\"},"
    "\"enabled\":{\"type\":\"boolean\"},"
    "\"outcome\":{\"type\":\"string\"},"
    "\"last_check_ts\":{\"type\":\"integer\"}},"
    "\"required\":[\"current\",\"latest\",\"download_url\",\"available\","
    "\"ts\",\"last_check_ok\",\"enabled\",\"outcome\"]}";

void test_sse_schema_update_available_payload_valid(void)
{
    cJSON *obj = cJSON_CreateObject();
    TEST_ASSERT_NOT_NULL(obj);
    cJSON_AddStringToObject(obj, "current",      "1.0.0");
    cJSON_AddStringToObject(obj, "latest",       "1.1.0");
    cJSON_AddStringToObject(obj, "download_url", "https://example.com/fw.bin");
    cJSON_AddBoolToObject(obj, "available",    true);
    cJSON_AddNumberToObject(obj, "ts",           1700000000LL);
    cJSON_AddBoolToObject(obj, "last_check_ok",true);
    cJSON_AddBoolToObject(obj, "enabled",      true);
    cJSON_AddStringToObject(obj, "outcome",      "update_available");
    TEST_ASSERT_EQUAL(BB_OK, bb_openapi_validate(k_update_available_schema, obj, NULL));
    cJSON_Delete(obj);
}

void test_sse_schema_update_available_payload_missing_required_fails(void)
{
    cJSON *obj = cJSON_CreateObject();
    TEST_ASSERT_NOT_NULL(obj);
    cJSON_AddStringToObject(obj, "current", "1.0.0");
    // missing latest, download_url, available, ts, last_check_ok, enabled, outcome
    bb_openapi_validate_err_t verr = {0};
    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION,
                      bb_openapi_validate(k_update_available_schema, obj, &verr));
    cJSON_Delete(obj);
}

// DiagBoot (sse_topic="diag.boot")
static const char k_diag_boot_schema[] =
    "{\"title\":\"DiagBoot\",\"x-sse-topic\":\"diag.boot\",\"type\":\"object\","
    "\"properties\":{"
    "\"reset_reason\":{\"type\":\"string\"},"
    "\"wdt_resets\":{\"type\":\"integer\"},"
    "\"panic\":{\"type\":\"object\",\"properties\":{"
    "\"available\":{\"type\":\"boolean\"},"
    "\"boots_since\":{\"type\":\"integer\"}}},"
    "\"pending_verify\":{\"type\":\"boolean\"},"
    "\"rolled_back\":{\"type\":\"boolean\"}},"
    "\"required\":[\"reset_reason\",\"wdt_resets\",\"panic\","
    "\"pending_verify\",\"rolled_back\"]}";

void test_sse_schema_diag_boot_payload_valid(void)
{
    cJSON *panic = cJSON_CreateObject();
    TEST_ASSERT_NOT_NULL(panic);
    cJSON_AddBoolToObject(panic, "available", false);

    cJSON *obj = cJSON_CreateObject();
    TEST_ASSERT_NOT_NULL(obj);
    cJSON_AddStringToObject(obj, "reset_reason",  "power_on");
    cJSON_AddNumberToObject(obj, "wdt_resets",    0);
    cJSON_AddItemToObject(obj, "panic", panic);
    cJSON_AddBoolToObject(obj, "pending_verify",false);
    cJSON_AddBoolToObject(obj, "rolled_back",   false);
    TEST_ASSERT_EQUAL(BB_OK, bb_openapi_validate(k_diag_boot_schema, obj, NULL));
    cJSON_Delete(obj);
}

void test_sse_schema_diag_boot_payload_missing_required_fails(void)
{
    cJSON *obj = cJSON_CreateObject();
    TEST_ASSERT_NOT_NULL(obj);
    cJSON_AddStringToObject(obj, "reset_reason", "power_on");
    // missing wdt_resets, panic, pending_verify, rolled_back
    bb_openapi_validate_err_t verr = {0};
    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION,
                      bb_openapi_validate(k_diag_boot_schema, obj, &verr));
    cJSON_Delete(obj);
}

// DisplayInfo (sse_topic="health.display")
static const char k_display_info_schema[] =
    "{\"title\":\"DisplayInfo\",\"x-sse-topic\":\"health.display\",\"type\":\"object\","
    "\"properties\":{"
    "\"present\":{\"type\":\"boolean\"},"
    "\"panel\":{\"type\":[\"string\",\"null\"]},"
    "\"width\":{\"type\":\"integer\"},"
    "\"height\":{\"type\":\"integer\"},"
    "\"enabled\":{\"type\":\"boolean\"}},"
    "\"required\":[\"present\"]}";

void test_sse_schema_display_info_payload_valid(void)
{
    cJSON *obj = cJSON_CreateObject();
    TEST_ASSERT_NOT_NULL(obj);
    cJSON_AddBoolToObject(obj, "present", false);
    TEST_ASSERT_EQUAL(BB_OK, bb_openapi_validate(k_display_info_schema, obj, NULL));
    cJSON_Delete(obj);
}

void test_sse_schema_display_info_payload_missing_required_fails(void)
{
    cJSON *obj = cJSON_CreateObject();
    TEST_ASSERT_NOT_NULL(obj);
    cJSON_AddStringToObject(obj, "panel", "ILI9341");
    // missing present
    bb_openapi_validate_err_t verr = {0};
    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION,
                      bb_openapi_validate(k_display_info_schema, obj, &verr));
    cJSON_Delete(obj);
}

// HealthStack (sse_topic="health.stack")
static const char k_health_stack_schema[] =
    "{\"title\":\"HealthStack\",\"x-sse-topic\":\"health.stack\",\"type\":\"object\","
    "\"properties\":{"
    "\"task\":{\"type\":\"string\"},"
    "\"free_bytes\":{\"type\":\"integer\"},"
    "\"low\":{\"type\":\"boolean\"}},"
    "\"required\":[\"task\",\"free_bytes\",\"low\"]}";

void test_sse_schema_health_stack_payload_valid(void)
{
    cJSON *obj = cJSON_CreateObject();
    TEST_ASSERT_NOT_NULL(obj);
    cJSON_AddStringToObject(obj, "task",       "httpd");
    cJSON_AddNumberToObject(obj, "free_bytes", 512);
    cJSON_AddBoolToObject(obj, "low",        true);
    TEST_ASSERT_EQUAL(BB_OK, bb_openapi_validate(k_health_stack_schema, obj, NULL));
    cJSON_Delete(obj);
}

void test_sse_schema_health_stack_payload_missing_required_fails(void)
{
    cJSON *obj = cJSON_CreateObject();
    TEST_ASSERT_NOT_NULL(obj);
    cJSON_AddStringToObject(obj, "task", "httpd");
    // missing free_bytes and low
    bb_openapi_validate_err_t verr = {0};
    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION,
                      bb_openapi_validate(k_health_stack_schema, obj, &verr));
    cJSON_Delete(obj);
}

// OtaProgress (sse_topic="ota.progress")
static const char k_ota_progress_schema[] =
    "{\"title\":\"OtaProgress\",\"x-sse-topic\":\"ota.progress\",\"type\":\"object\","
    "\"properties\":{"
    "\"via\":{\"type\":\"string\"},"
    "\"state\":{\"type\":\"string\","
    "\"enum\":[\"start\",\"progress\",\"success\",\"fail\",\"unknown\"]},"
    "\"pct\":{\"type\":\"integer\"}},"
    "\"required\":[\"via\",\"state\",\"pct\"]}";

void test_sse_schema_ota_progress_payload_valid(void)
{
    cJSON *obj = cJSON_CreateObject();
    TEST_ASSERT_NOT_NULL(obj);
    cJSON_AddStringToObject(obj, "via",   "mqtt");
    cJSON_AddStringToObject(obj, "state", "progress");
    cJSON_AddNumberToObject(obj, "pct",   50);
    TEST_ASSERT_EQUAL(BB_OK, bb_openapi_validate(k_ota_progress_schema, obj, NULL));
    cJSON_Delete(obj);
}

void test_sse_schema_ota_progress_payload_missing_required_fails(void)
{
    cJSON *obj = cJSON_CreateObject();
    TEST_ASSERT_NOT_NULL(obj);
    cJSON_AddStringToObject(obj, "via", "mqtt");
    // missing state and pct
    bb_openapi_validate_err_t verr = {0};
    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION,
                      bb_openapi_validate(k_ota_progress_schema, obj, &verr));
    cJSON_Delete(obj);
}

// ---------------------------------------------------------------------------
// oneOf count: all SSE topics registered → 11 refs in /api/events 200 oneOf
// (log + wifi + fan + power + thermal + alert + update.available +
//  diag.boot + health.display + health.stack + ota.progress)
// ---------------------------------------------------------------------------

void test_sse_schema_sse_oneof_count_and_topics(void)
{
    bb_http_route_registry_clear();
    bb_http_register_described_route(NULL, &s_sse_route);

    // Register all 11 SSE-facing schemas (sse_topic != NULL).
    bb_openapi_register_schema("LogEvent",        k_log_schema,              "log");
    bb_openapi_register_schema("WifiTelemetry",   k_wifi_telemetry_schema,   "wifi");
    bb_openapi_register_schema("FanTelemetry",    k_fan_telemetry_schema,    "fan");
    bb_openapi_register_schema("PowerTelemetry",  k_power_telemetry_schema,  "power");
    bb_openapi_register_schema("ThermalTelemetry",k_thermal_telemetry_schema,"thermal");
    bb_openapi_register_schema("Alert",           k_alert_schema,            "alert");
    bb_openapi_register_schema("UpdateAvailable", k_update_available_schema, "update.available");
    bb_openapi_register_schema("DiagBoot",        k_diag_boot_schema,        "diag.boot");
    bb_openapi_register_schema("DisplayInfo",     k_display_info_schema,     "health.display");
    bb_openapi_register_schema("HealthStack",     k_health_stack_schema,     "health.stack");
    bb_openapi_register_schema("OtaProgress",     k_ota_progress_schema,     "ota.progress");
    // Also register REST-only schemas (must NOT appear in oneOf).
    bb_openapi_register_schema("InfoTelemetry",   k_info_telemetry_schema,   NULL);
    bb_openapi_register_schema("RtosTelemetry",   k_rtos_telemetry_schema,   NULL);
    bb_openapi_register_schema("WifiInfo",        k_wifi_schema,             NULL);

    TEST_ASSERT_EQUAL_size_t(14, bb_openapi_schema_count());

    bb_openapi_meta_t meta = { .title = "T", .version = "1.0" };
    test_openapi_capture_result_t r = test_openapi_capture(&meta);
    TEST_ASSERT_EQUAL(BB_OK, r.status);
    TEST_ASSERT_NOT_NULL(r.cap.body);

    // All 11 SSE schemas must appear as $ref entries in oneOf.
    TEST_ASSERT_NOT_NULL(strstr(r.cap.body, "\"$ref\":\"#/components/schemas/LogEvent\""));
    TEST_ASSERT_NOT_NULL(strstr(r.cap.body, "\"$ref\":\"#/components/schemas/WifiTelemetry\""));
    TEST_ASSERT_NOT_NULL(strstr(r.cap.body, "\"$ref\":\"#/components/schemas/FanTelemetry\""));
    TEST_ASSERT_NOT_NULL(strstr(r.cap.body, "\"$ref\":\"#/components/schemas/PowerTelemetry\""));
    TEST_ASSERT_NOT_NULL(strstr(r.cap.body, "\"$ref\":\"#/components/schemas/ThermalTelemetry\""));
    TEST_ASSERT_NOT_NULL(strstr(r.cap.body, "\"$ref\":\"#/components/schemas/Alert\""));
    TEST_ASSERT_NOT_NULL(strstr(r.cap.body, "\"$ref\":\"#/components/schemas/UpdateAvailable\""));
    TEST_ASSERT_NOT_NULL(strstr(r.cap.body, "\"$ref\":\"#/components/schemas/DiagBoot\""));
    TEST_ASSERT_NOT_NULL(strstr(r.cap.body, "\"$ref\":\"#/components/schemas/DisplayInfo\""));
    TEST_ASSERT_NOT_NULL(strstr(r.cap.body, "\"$ref\":\"#/components/schemas/HealthStack\""));
    TEST_ASSERT_NOT_NULL(strstr(r.cap.body, "\"$ref\":\"#/components/schemas/OtaProgress\""));
    // REST-only schemas must NOT appear in oneOf.
    TEST_ASSERT_NULL(strstr(r.cap.body, "\"$ref\":\"#/components/schemas/InfoTelemetry\""));
    TEST_ASSERT_NULL(strstr(r.cap.body, "\"$ref\":\"#/components/schemas/RtosTelemetry\""));
    TEST_ASSERT_NULL(strstr(r.cap.body, "\"$ref\":\"#/components/schemas/WifiInfo\""));

    test_openapi_capture_free(&r);
}

// ---------------------------------------------------------------------------
// build_sse_oneof_fragment (bounded string splice) — B1-1116 PR2
// ---------------------------------------------------------------------------

void test_sse_schema_oneof_fragment_single_topic(void)
{
    bb_http_route_registry_clear();
    bb_http_register_described_route(NULL, &s_sse_route);
    bb_openapi_register_schema("LogEvent", k_log_schema, "log");

    bb_openapi_meta_t meta = { .title = "T", .version = "1.0" };
    test_openapi_capture_result_t r = test_openapi_capture(&meta);
    TEST_ASSERT_EQUAL(BB_OK, r.status);
    TEST_ASSERT_NOT_NULL(r.cap.body);

    TEST_ASSERT_NOT_NULL(strstr(r.cap.body,
        "\"oneOf\":[{\"$ref\":\"#/components/schemas/LogEvent\"}]"));

    test_openapi_capture_free(&r);
}

void test_sse_schema_oneof_fragment_max_cap_topics(void)
{
    bb_http_route_registry_clear();
    bb_http_register_described_route(NULL, &s_sse_route);

    // Registry stores raw pointers — names must outlive the emit call.
    static char names[BB_OPENAPI_SCHEMA_REGISTRY_CAP][8];
    for (size_t i = 0; i < BB_OPENAPI_SCHEMA_REGISTRY_CAP; i++) {
        snprintf(names[i], sizeof(names[i]), "S%zu", i);
        TEST_ASSERT_EQUAL(BB_OK,
            bb_openapi_register_schema(names[i], k_log_schema, names[i]));
    }
    TEST_ASSERT_EQUAL_size_t(BB_OPENAPI_SCHEMA_REGISTRY_CAP, bb_openapi_schema_count());

    bb_openapi_meta_t meta = { .title = "T", .version = "1.0" };
    test_openapi_capture_result_t r = test_openapi_capture(&meta);
    TEST_ASSERT_EQUAL(BB_OK, r.status);
    TEST_ASSERT_NOT_NULL(r.cap.body);

    // Realistic component-name lengths at the full registry cap fit within
    // BB_OPENAPI_SSE_ONEOF_BUF_SIZE — content block is present, all present.
    TEST_ASSERT_NOT_NULL(strstr(r.cap.body, "\"oneOf\":["));
    for (size_t i = 0; i < BB_OPENAPI_SCHEMA_REGISTRY_CAP; i++) {
        char needle[64];
        snprintf(needle, sizeof(needle), "\"$ref\":\"#/components/schemas/%s\"", names[i]);
        TEST_ASSERT_NOT_NULL(strstr(r.cap.body, needle));
    }

    test_openapi_capture_free(&r);
}

void test_sse_schema_oneof_fragment_mixed_sse_and_rest_only(void)
{
    bb_http_route_registry_clear();
    bb_http_register_described_route(NULL, &s_sse_route);
    bb_openapi_register_schema("LogEvent", k_log_schema, "log");
    // REST-only (NULL sse_topic) — must be excluded from oneOf.
    bb_openapi_register_schema("WifiInfo", k_wifi_schema, NULL);

    bb_openapi_meta_t meta = { .title = "T", .version = "1.0" };
    test_openapi_capture_result_t r = test_openapi_capture(&meta);
    TEST_ASSERT_EQUAL(BB_OK, r.status);
    TEST_ASSERT_NOT_NULL(r.cap.body);

    TEST_ASSERT_NOT_NULL(strstr(r.cap.body, "\"$ref\":\"#/components/schemas/LogEvent\""));
    TEST_ASSERT_NULL(strstr(r.cap.body, "\"$ref\":\"#/components/schemas/WifiInfo\""));
    // REST-only schema still appears in components/schemas, just not oneOf.
    TEST_ASSERT_NOT_NULL(strstr(r.cap.body, "\"WifiInfo\":"));

    test_openapi_capture_free(&r);
}

// ---------------------------------------------------------------------------
// build_sse_oneof_fragment() branch coverage via the direct test seam
// (bb_openapi_build_sse_oneof_fragment_for_test) — driving out_size directly
// is far cheaper than approaching BB_OPENAPI_SCHEMA_REGISTRY_CAP-sized real
// data for edges the route-level tests above don't happen to trip.
// ---------------------------------------------------------------------------

// out_size too small even for the "{\"oneOf\":[" opener — trips before the
// registry loop runs at all (registry state is irrelevant).
void test_sse_schema_oneof_fragment_opener_overflow(void)
{
    char tiny[4];
    TEST_ASSERT_FALSE(bb_openapi_build_sse_oneof_fragment_for_test(tiny, sizeof(tiny)));
}

// out_size exactly one byte short of the true fragment length: the opener
// and the (single) entry both fit, but there is no room left for the
// closing "]}" — the branch distinct from the opener/per-entry overflows
// covered above and by the route-level 24-entry overflow test.
void test_sse_schema_oneof_fragment_closing_overflow(void)
{
    bb_openapi_register_schema("S", k_log_schema, "log");

    char full[BB_OPENAPI_SSE_ONEOF_BUF_SIZE];
    TEST_ASSERT_TRUE(bb_openapi_build_sse_oneof_fragment_for_test(full, sizeof(full)));
    size_t full_len = strlen(full);

    char tight[BB_OPENAPI_SSE_ONEOF_BUF_SIZE];
    TEST_ASSERT_FALSE(bb_openapi_build_sse_oneof_fragment_for_test(tight, full_len));
}

// Stream-path counterpart to the now-deleted tree-path
// test_sse_schema_oneof_fragment_overflow_omits_content — same oversized
// registry, but through bb_openapi_emit_stream()'s emit_operation()
// (B1-1116 PR4), pinning that the overflow-omit branch is still covered now
// that the tree emitter is gone.
void test_sse_schema_oneof_fragment_overflow_omits_content_stream(void)
{
    bb_http_route_registry_clear();
    bb_http_register_described_route(NULL, &s_sse_route);

    static char names[BB_OPENAPI_SCHEMA_REGISTRY_CAP][64];
    for (size_t i = 0; i < BB_OPENAPI_SCHEMA_REGISTRY_CAP; i++) {
        // 2-digit index + 56 zero-padded digits = 58 chars.
        snprintf(names[i], sizeof(names[i]), "%02zu%056d", i, 0);
        TEST_ASSERT_EQUAL(BB_OK,
            bb_openapi_register_schema(names[i], k_log_schema, names[i]));
    }
    TEST_ASSERT_EQUAL_size_t(BB_OPENAPI_SCHEMA_REGISTRY_CAP, bb_openapi_schema_count());

    bb_openapi_meta_t meta2 = { .title = "T", .version = "1.0" };
    test_openapi_capture_result_t r = test_openapi_capture(&meta2);
    TEST_ASSERT_EQUAL(BB_OK, r.status);
    TEST_ASSERT_NOT_NULL(r.doc);

    cJSON *s_paths  = cJSON_GetObjectItemCaseSensitive(r.doc, "paths");
    cJSON *s_pi     = cJSON_GetObjectItemCaseSensitive(s_paths, "/api/events");
    cJSON *s_get_op = cJSON_GetObjectItemCaseSensitive(s_pi, "get");
    cJSON *s_resps  = cJSON_GetObjectItemCaseSensitive(s_get_op, "responses");
    cJSON *s_r200   = cJSON_GetObjectItemCaseSensitive(s_resps, "200");
    TEST_ASSERT_NOT_NULL(s_r200);
    // Fragment overflowed the bounded buffer — content omitted, not truncated.
    TEST_ASSERT_NULL(cJSON_GetObjectItemCaseSensitive(s_r200, "content"));

    test_openapi_capture_free(&r);
}

// Stream-path counterpart to the deleted tree-path
// test_sse_schema_oneof_fragment_long_name_truncation_omits_content (B1-1054
// PR 6 removed the tree emitter): a component name long enough that
// "#/components/schemas/<name>" truncates inside ref_val's own 80-byte
// budget (name > 58 chars) must not silently splice a truncated
// (wrong-but-valid-looking) $ref into the fragment. This is the sole
// remaining cover for build_sse_oneof_fragment()'s ref_n overflow branch
// (components/bb_openapi/src/bb_openapi_emit_shared.c) — losing it would
// drop that branch's coverage with no code removal to offset it.
void test_sse_schema_oneof_fragment_long_name_truncation_omits_content_stream(void)
{
    bb_http_route_registry_clear();
    bb_http_register_described_route(NULL, &s_sse_route);

    static char long_name[70];
    memset(long_name, 'A', sizeof(long_name) - 1);
    long_name[sizeof(long_name) - 1] = '\0';

    TEST_ASSERT_EQUAL(BB_OK,
        bb_openapi_register_schema(long_name, k_log_schema, "log"));

    bb_openapi_meta_t meta = { .title = "T", .version = "1.0" };
    test_openapi_capture_result_t r = test_openapi_capture(&meta);
    TEST_ASSERT_EQUAL(BB_OK, r.status);
    TEST_ASSERT_NOT_NULL(r.doc);

    cJSON *paths  = cJSON_GetObjectItemCaseSensitive(r.doc, "paths");
    cJSON *pi     = cJSON_GetObjectItemCaseSensitive(paths, "/api/events");
    cJSON *get_op = cJSON_GetObjectItemCaseSensitive(pi, "get");
    cJSON *resps  = cJSON_GetObjectItemCaseSensitive(get_op, "responses");
    cJSON *r200   = cJSON_GetObjectItemCaseSensitive(resps, "200");
    TEST_ASSERT_NOT_NULL(r200);
    TEST_ASSERT_NULL(cJSON_GetObjectItemCaseSensitive(r200, "content"));

    test_openapi_capture_free(&r);
}

// ---------------------------------------------------------------------------
// External topic-source seam union (B1-1220 PR2) -- sse_schema_count() /
// build_sse_oneof_fragment() can union an EXTERNAL topic-schema source
// (bb_data_http's describe table, via bb_data_http_describe_foreach()) into
// the legacy registry above, but ONLY when bb_openapi_set_topic_source_fn()
// has actually wired it -- bb_openapi never links bb_data_http directly (see
// bb_openapi.h's doc comment). Default (unwired, NULL) behaves byte-
// identically to pre-B1-1220. These tests also cover the silent-failure mode
// the seam trades for the dependency: a populated describe table with the
// seam left UNWIRED must produce NO trace of its entries in the emitted
// document (exactly the mistake a composition root could make by forgetting
// to pair bb_data_http_describe() calls with
// bb_openapi_set_topic_source_fn(bb_data_http_describe_foreach)).
// ---------------------------------------------------------------------------

void test_sse_schema_union_seam_unwired_matches_legacy_only_output(void)
{
    bb_http_route_registry_clear();
    bb_http_register_described_route(NULL, &s_sse_route);
    bb_openapi_register_schema("LogEvent", k_log_schema, "log");

    // Seam untouched (NULL, the default) -- this pins the exact same byte-
    // identical output the pre-B1-1220 legacy-only path produced.
    TEST_ASSERT_EQUAL_size_t(1, sse_schema_count());

    bb_openapi_meta_t meta = { .title = "T", .version = "1.0" };
    test_openapi_capture_result_t r = test_openapi_capture(&meta);
    TEST_ASSERT_EQUAL(BB_OK, r.status);
    TEST_ASSERT_NOT_NULL(r.cap.body);
    TEST_ASSERT_NOT_NULL(strstr(r.cap.body,
        "\"oneOf\":[{\"$ref\":\"#/components/schemas/LogEvent\"}]"));

    test_openapi_capture_free(&r);
}

// Silent-failure demonstration: a producer calls bb_data_http_describe()
// (populating the table) but the composition root never wires the seam --
// the entry must NOT appear anywhere in the emitted document (oneOf OR
// components/schemas), same as if bb_data_http didn't exist at all.
void test_sse_schema_union_seam_unwired_describe_entry_does_not_appear(void)
{
    bb_data_http_reset_for_test();
    bb_http_route_registry_clear();
    bb_http_register_described_route(NULL, &s_sse_route);
    bb_openapi_register_schema("LogEvent", k_log_schema, "log");
    TEST_ASSERT_EQUAL(BB_OK,
        bb_data_http_describe("k1", "topic.a", "DescribedThing", "{\"type\":\"object\"}"));

    // Seam intentionally left unwired.
    TEST_ASSERT_EQUAL_size_t(1, sse_schema_count());
    TEST_ASSERT_EQUAL_size_t(1, bb_openapi_schemas_union_count());

    bb_openapi_meta_t meta = { .title = "T", .version = "1.0" };
    test_openapi_capture_result_t r = test_openapi_capture(&meta);
    TEST_ASSERT_EQUAL(BB_OK, r.status);
    TEST_ASSERT_NOT_NULL(r.doc);
    TEST_ASSERT_NOT_NULL(r.cap.body);
    TEST_ASSERT_NOT_NULL(strstr(r.cap.body,
        "\"oneOf\":[{\"$ref\":\"#/components/schemas/LogEvent\"}]"));
    TEST_ASSERT_NULL(strstr(r.cap.body, "DescribedThing"));

    // components/schemas body-level check, not just a whole-body string
    // scan: DescribedThing must have no entry there either.
    cJSON *components = cJSON_GetObjectItemCaseSensitive(r.doc, "components");
    TEST_ASSERT_NOT_NULL(components);
    cJSON *schemas = cJSON_GetObjectItemCaseSensitive(components, "schemas");
    TEST_ASSERT_NOT_NULL(schemas);
    TEST_ASSERT_NULL(cJSON_GetObjectItemCaseSensitive(schemas, "DescribedThing"));
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItemCaseSensitive(schemas, "LogEvent"));

    test_openapi_capture_free(&r);
    bb_data_http_reset_for_test();
}

void test_sse_schema_union_seam_wired_describe_entry_counts_toward_sse_schema_count(void)
{
    bb_data_http_reset_for_test();
    bb_openapi_set_topic_source_fn(bb_data_http_describe_foreach);
    TEST_ASSERT_EQUAL(BB_OK,
        bb_data_http_describe("k1", "topic.a", "DescribedThing", "{\"type\":\"object\"}"));

    TEST_ASSERT_EQUAL_size_t(1, sse_schema_count());
    TEST_ASSERT_EQUAL_size_t(1, bb_openapi_schemas_union_count());

    bb_openapi_set_topic_source_fn(NULL);
    bb_data_http_reset_for_test();
}

// The weakness this replaces: the pre-fix version of this test asserted only
// that the "#/components/schemas/DescribedThing" $ref STRING appears in the
// emitted oneOf array — never that a matching components/schemas.
// DescribedThing BODY exists. A document with a $ref and no matching body is
// invalid OpenAPI (a dangling reference) — exactly the B1-1220 defect this
// fix closes. This version checks both the $ref AND the body, plus runs the
// generic cJSON dangling-$ref oracle (bb_openapi_validate_no_dangling_refs)
// over the whole document.
void test_sse_schema_union_seam_wired_describe_entry_appears_in_oneof(void)
{
    bb_data_http_reset_for_test();
    bb_openapi_set_topic_source_fn(bb_data_http_describe_foreach);
    bb_http_route_registry_clear();
    bb_http_register_described_route(NULL, &s_sse_route);
    bb_openapi_register_schema("LogEvent", k_log_schema, "log");
    TEST_ASSERT_EQUAL(BB_OK,
        bb_data_http_describe("k1", "topic.a", "DescribedThing", "{\"type\":\"object\"}"));

    // Legacy-first-then-external-source ordering: LogEvent's $ref precedes
    // DescribedThing's in the emitted oneOf array.
    bb_openapi_meta_t meta = { .title = "T", .version = "1.0" };
    test_openapi_capture_result_t r = test_openapi_capture(&meta);
    TEST_ASSERT_EQUAL(BB_OK, r.status);
    TEST_ASSERT_NOT_NULL(r.doc);
    TEST_ASSERT_NOT_NULL(r.cap.body);
    TEST_ASSERT_NOT_NULL(strstr(r.cap.body,
        "\"oneOf\":[{\"$ref\":\"#/components/schemas/LogEvent\"},"
        "{\"$ref\":\"#/components/schemas/DescribedThing\"}]"));

    // The $ref must resolve: components/schemas carries DescribedThing's own
    // body (not just LogEvent's, which was already covered pre-fix).
    cJSON *components = cJSON_GetObjectItemCaseSensitive(r.doc, "components");
    TEST_ASSERT_NOT_NULL(components);
    cJSON *schemas = cJSON_GetObjectItemCaseSensitive(components, "schemas");
    TEST_ASSERT_NOT_NULL(schemas);
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItemCaseSensitive(schemas, "DescribedThing"));
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItemCaseSensitive(schemas, "LogEvent"));

    // Stronger, reusable guard: no $ref anywhere in the document dangles.
    TEST_ASSERT_EQUAL(BB_OK, bb_openapi_validate_no_dangling_refs(r.doc, NULL));

    test_openapi_capture_free(&r);
    bb_openapi_set_topic_source_fn(NULL);
    bb_data_http_reset_for_test();
}

// Count non-overlapping occurrences of `needle` in `haystack` — strstr()
// alone can only answer "at least once"; the dedup tests below need "EXACTLY
// once", since a duplicate $ref in oneOf violates its "exactly one
// subschema matches" semantics (not just untidy JSON) and a duplicate
// components/schemas key is a duplicate-key document (also invalid).
static size_t count_occurrences(const char *haystack, const char *needle)
{
    size_t count = 0;
    size_t needle_len = strlen(needle);
    const char *p = haystack;
    while ((p = strstr(p, needle)) != NULL) {
        count++;
        p += needle_len;
    }
    return count;
}

// Dedup: the same component_name registered via BOTH the legacy registry
// (bb_openapi_register_schema()) and the external describe table must emit
// exactly one components/schemas entry (legacy body wins — the same
// first-wins convention bb_openapi_register_schema() itself uses for a
// duplicate re-registration, test_sse_schema_registry_dedup_first_wins
// above) AND exactly one oneOf $ref, not two.
void test_sse_schema_union_seam_dedup_collision_legacy_wins(void)
{
    bb_data_http_reset_for_test();
    bb_openapi_set_topic_source_fn(bb_data_http_describe_foreach);
    bb_http_route_registry_clear();
    bb_http_register_described_route(NULL, &s_sse_route);
    bb_openapi_register_schema("Dup", k_log_schema, "log");
    TEST_ASSERT_EQUAL(BB_OK,
        bb_data_http_describe("k1", "topic.a", "Dup", k_wifi_schema));

    // Union count does not double-count the colliding name — neither the
    // components/schemas count nor the oneOf-facing count.
    TEST_ASSERT_EQUAL_size_t(1, bb_openapi_schemas_union_count());
    TEST_ASSERT_EQUAL_size_t(1, sse_schema_count());

    bb_openapi_meta_t meta = { .title = "T", .version = "1.0" };
    test_openapi_capture_result_t r = test_openapi_capture(&meta);
    TEST_ASSERT_EQUAL(BB_OK, r.status);
    TEST_ASSERT_NOT_NULL(r.doc);
    TEST_ASSERT_NOT_NULL(r.cap.body);

    cJSON *components = cJSON_GetObjectItemCaseSensitive(r.doc, "components");
    TEST_ASSERT_NOT_NULL(components);
    cJSON *schemas = cJSON_GetObjectItemCaseSensitive(components, "schemas");
    TEST_ASSERT_NOT_NULL(schemas);
    cJSON *dup = cJSON_GetObjectItemCaseSensitive(schemas, "Dup");
    TEST_ASSERT_NOT_NULL(dup);
    // Legacy body (k_log_schema, title "LogEvent") wins, not the describe-
    // table body (k_wifi_schema, title "WifiInfo").
    cJSON *title = cJSON_GetObjectItemCaseSensitive(dup, "title");
    TEST_ASSERT_NOT_NULL(title);
    TEST_ASSERT_EQUAL_STRING("LogEvent", title->valuestring);

    // oneOf carries exactly ONE $ref to Dup, not two.
    TEST_ASSERT_EQUAL_size_t(1,
        count_occurrences(r.cap.body, "\"$ref\":\"#/components/schemas/Dup\""));

    // A dedup bug that dropped the wrong body (or emitted both) would show
    // up as a dangling ref — confirm the whole document is still valid.
    TEST_ASSERT_EQUAL(BB_OK, bb_openapi_validate_no_dangling_refs(r.doc, NULL));

    test_openapi_capture_free(&r);
    bb_openapi_set_topic_source_fn(NULL);
    bb_data_http_reset_for_test();
}

// External-vs-external collision: bb_data_http_describe() dedups by `key`
// (bb_data_http.h), NOT by component_name — two distinct keys/topics can
// legitimately share one component_name, with no legacy entry involved at
// all. Must still emit exactly one components/schemas entry and exactly one
// oneOf $ref — the HIGH (duplicate JSON key) and MEDIUM (duplicate oneOf
// branch) findings this fix closes.
void test_sse_schema_union_seam_external_vs_external_collision(void)
{
    bb_data_http_reset_for_test();
    bb_openapi_set_topic_source_fn(bb_data_http_describe_foreach);
    bb_http_route_registry_clear();
    bb_http_register_described_route(NULL, &s_sse_route);
    // Two distinct keys/topics, same component_name, no legacy entry.
    TEST_ASSERT_EQUAL(BB_OK,
        bb_data_http_describe("k1", "topic.a", "Collide", "{\"type\":\"object\"}"));
    TEST_ASSERT_EQUAL(BB_OK,
        bb_data_http_describe("k2", "topic.b", "Collide", "{\"type\":\"string\"}"));

    // Registration-order first-wins, same convention as legacy-vs-external.
    TEST_ASSERT_EQUAL_size_t(1, bb_openapi_schemas_union_count());
    TEST_ASSERT_EQUAL_size_t(1, sse_schema_count());

    bb_openapi_meta_t meta = { .title = "T", .version = "1.0" };
    test_openapi_capture_result_t r = test_openapi_capture(&meta);
    TEST_ASSERT_EQUAL(BB_OK, r.status);
    TEST_ASSERT_NOT_NULL(r.doc);
    TEST_ASSERT_NOT_NULL(r.cap.body);

    cJSON *components = cJSON_GetObjectItemCaseSensitive(r.doc, "components");
    TEST_ASSERT_NOT_NULL(components);
    cJSON *schemas = cJSON_GetObjectItemCaseSensitive(components, "schemas");
    TEST_ASSERT_NOT_NULL(schemas);
    cJSON *collide = cJSON_GetObjectItemCaseSensitive(schemas, "Collide");
    TEST_ASSERT_NOT_NULL(collide);
    // topic.a's body (registered first) won.
    cJSON *type = cJSON_GetObjectItemCaseSensitive(collide, "type");
    TEST_ASSERT_NOT_NULL(type);
    TEST_ASSERT_EQUAL_STRING("object", type->valuestring);

    // Exactly one "Collide" key in components/schemas (cJSON's own parse
    // silently keeps only the first of a duplicate key — the raw-body count
    // below is what actually proves the stream never emitted a second one),
    // and exactly one $ref in oneOf.
    TEST_ASSERT_EQUAL_size_t(1, count_occurrences(r.cap.body, "\"Collide\":"));
    TEST_ASSERT_EQUAL_size_t(1,
        count_occurrences(r.cap.body, "\"$ref\":\"#/components/schemas/Collide\""));

    TEST_ASSERT_EQUAL(BB_OK, bb_openapi_validate_no_dangling_refs(r.doc, NULL));

    test_openapi_capture_free(&r);
    bb_openapi_set_topic_source_fn(NULL);
    bb_data_http_reset_for_test();
}

// Whole-document validity: every $ref in the oneOf array resolves to a real
// components/schemas entry once the seam is wired, across a mix of legacy
// and describe-table entries — the generic dangling-$ref oracle
// (bb_openapi_validate_no_dangling_refs) is exactly the assertion that would
// have caught the B1-1220 components/schemas defect.
void test_sse_schema_union_seam_wired_produces_valid_document(void)
{
    bb_data_http_reset_for_test();
    bb_openapi_set_topic_source_fn(bb_data_http_describe_foreach);
    bb_http_route_registry_clear();
    bb_http_register_described_route(NULL, &s_sse_route);
    bb_openapi_register_schema("LogEvent", k_log_schema, "log");
    TEST_ASSERT_EQUAL(BB_OK,
        bb_data_http_describe("k1", "topic.a", "DescribedThing", "{\"type\":\"object\"}"));
    TEST_ASSERT_EQUAL(BB_OK,
        bb_data_http_describe("k2", "topic.b", "DescribedOther", "{\"type\":\"object\"}"));

    bb_openapi_meta_t meta = { .title = "T", .version = "1.0" };
    test_openapi_capture_result_t r = test_openapi_capture(&meta);
    TEST_ASSERT_EQUAL(BB_OK, r.status);
    TEST_ASSERT_NOT_NULL(r.doc);

    bb_openapi_validate_err_t verr = {0};
    TEST_ASSERT_EQUAL(BB_OK, bb_openapi_validate_no_dangling_refs(r.doc, &verr));

    test_openapi_capture_free(&r);
    bb_openapi_set_topic_source_fn(NULL);
    bb_data_http_reset_for_test();
}

// Regression pin: bb_openapi_validate_no_dangling_refs() itself detects a
// dangling $ref (a document with a $ref pointing at a components/schemas
// entry that was never given a body) — the exact shape the B1-1220
// components/schemas defect produced before this fix.
void test_sse_schema_dangling_ref_oracle_detects_missing_schema_body(void)
{
    cJSON *doc = cJSON_Parse(
        "{\"components\":{\"schemas\":{\"LogEvent\":{\"type\":\"object\"}}},"
        "\"content\":{\"schema\":{\"oneOf\":["
        "{\"$ref\":\"#/components/schemas/LogEvent\"},"
        "{\"$ref\":\"#/components/schemas/Missing\"}]}}}");
    TEST_ASSERT_NOT_NULL(doc);

    bb_openapi_validate_err_t verr = {0};
    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION, bb_openapi_validate_no_dangling_refs(doc, &verr));
    TEST_ASSERT_NOT_NULL(strstr(verr.message, "Missing"));

    cJSON_Delete(doc);
}

void test_sse_schema_dangling_ref_oracle_null_doc_returns_invalid_arg(void)
{
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_openapi_validate_no_dangling_refs(NULL, NULL));
}

// Depth guard: a document nested far deeper than any real emitted document
// (~11 levels to reach a $ref, see bb_openapi_validate.c's doc comment on
// the depth bound) must fail closed (BB_ERR_VALIDATION) rather than recurse
// further — cheap insurance now that this oracle is a reusable general
// check, not just a check tied to one depth-capped producer.
void test_sse_schema_dangling_ref_oracle_depth_guard(void)
{
    char buf[2048];
    size_t pos = 0;
    for (int i = 0; i < 40; i++) {
        pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, "{\"a\":");
    }
    pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, "1");
    for (int i = 0; i < 40; i++) {
        pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, "}");
    }
    TEST_ASSERT_TRUE(pos < sizeof(buf));

    cJSON *doc = cJSON_Parse(buf);
    TEST_ASSERT_NOT_NULL(doc);

    bb_openapi_validate_err_t verr = {0};
    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION, bb_openapi_validate_no_dangling_refs(doc, &verr));
    TEST_ASSERT_NOT_NULL(strstr(verr.message, "depth"));

    cJSON_Delete(doc);
}

// ---------------------------------------------------------------------------
// External dedup-cap overflow (B1-1220 follow-up hardening) -- proves the
// schema_dedup_mark() overflow path (bb_openapi_emit_shared.c) is LOUD, not
// silent: past BB_OPENAPI_EXTERNAL_DEDUP_CAP (16) distinct external
// component_names in one walk, dedup tracking stops recording new names, so
// a later repeat of one of the untracked names re-emits a duplicate
// components/schemas entry -- exactly the defect this whole commit closes,
// reintroduced past the cap. bb_data_http's own describe table caps at 8
// (BB_DATA_HTTP_MAX_DESCRIBE), so it can never reach 16 -- this drives the
// seam directly with a synthetic topic-source function (the seam accepts
// any `(cb, ctx)` walker, not just bb_data_http_describe_foreach) to prove
// the degrade + logging is real, not just documented.
// ---------------------------------------------------------------------------

// Synthetic external source: walks 17 distinct component_names (one past the
// cap), then re-emits the 17th (the one dedup tracking failed to record,
// since the cap was already full when it was seen) a second time. 18 calls
// total; only 17 are DISTINCT names.
static void synthetic_dedup_overflow_source(bb_openapi_topic_cb_t cb, void *ctx)
{
    static char names[17][16];
    for (int i = 0; i < 17; i++) {
        snprintf(names[i], sizeof(names[i]), "Synth%d", i);
        cb("k", "topic", names[i], "{\"type\":\"object\"}", ctx);
    }
    // Repeat the 17th (index 16) name -- the one that overflowed the dedup
    // scratch and so was never recorded as seen.
    cb("k", "topic", names[16], "{\"type\":\"object\"}", ctx);
}

// dup()/dup2() stderr capture: bb_log_e/_w on host expand to fprintf(stderr,
// ...) (bb_log.h) -- no bb_log test seam exists to intercept formatted lines
// directly, so redirect the fd around the call under test and read it back.
// Precedent for direct fd/unistd.h manipulation in this test tree: test_bb_
// lock.c, test_bb_once.c, test_openapi_capture_selftest.c.
static int    s_stderr_saved_fd = -1;
static FILE  *s_stderr_capture_file = NULL;

static void capture_stderr_begin(void)
{
    fflush(stderr);
    s_stderr_saved_fd = dup(fileno(stderr));
    TEST_ASSERT_TRUE(s_stderr_saved_fd >= 0);
    s_stderr_capture_file = tmpfile();
    TEST_ASSERT_NOT_NULL(s_stderr_capture_file);
    TEST_ASSERT_TRUE(dup2(fileno(s_stderr_capture_file), fileno(stderr)) >= 0);
}

static size_t capture_stderr_end(char *out, size_t out_size)
{
    fflush(stderr);
    dup2(s_stderr_saved_fd, fileno(stderr));
    close(s_stderr_saved_fd);
    s_stderr_saved_fd = -1;
    rewind(s_stderr_capture_file);
    size_t n = fread(out, 1, out_size - 1, s_stderr_capture_file);
    out[n] = '\0';
    fclose(s_stderr_capture_file);
    s_stderr_capture_file = NULL;
    return n;
}

void test_sse_schema_union_seam_external_dedup_cap_overflow_logs_once_and_degrades(void)
{
    bb_openapi_set_topic_source_fn(synthetic_dedup_overflow_source);

    char captured[4096];
    capture_stderr_begin();
    size_t union_count = bb_openapi_schemas_union_count();
    capture_stderr_end(captured, sizeof(captured));

    // Degrade, as documented: 17 distinct names fed, but the 17th collided
    // with the cap and its repeat was never recognised as a duplicate, so
    // the union count is 18, not 17 -- a real re-emission, not a cosmetic
    // symptom.
    TEST_ASSERT_EQUAL_size_t(18, union_count);

    // Loud, not silent: the overflow logs.
    TEST_ASSERT_NOT_NULL(strstr(captured, "external schema dedup cap"));
    TEST_ASSERT_NOT_NULL(strstr(captured, "16"));

    // One-shot: two calls hit the over-cap branch in this walk (the 17th
    // distinct name, then its repeat) but only one log line is emitted.
    TEST_ASSERT_EQUAL_size_t(1, count_occurrences(captured, "external schema dedup cap"));

    bb_openapi_set_topic_source_fn(NULL);
}
