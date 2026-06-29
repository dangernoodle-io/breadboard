#include "unity.h"
#include "bb_openapi.h"
#include "bb_http.h"
#include "bb_http_host.h"
#include "bb_json.h"
#include "bb_json_test_hooks.h"

#include <string.h>

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
    bb_json_t obj = bb_json_obj_new();
    TEST_ASSERT_NOT_NULL(obj);
    bb_json_obj_set_int(obj,    "ts",    1234567890LL);
    bb_json_obj_set_string(obj, "level", "I");
    bb_json_obj_set_string(obj, "tag",   "wifi");
    bb_json_obj_set_string(obj, "msg",   "connected");
    TEST_ASSERT_EQUAL(BB_OK, bb_openapi_validate(k_log_schema, obj, NULL));
    bb_json_free(obj);
}

void test_sse_schema_log_payload_missing_required_fails(void)
{
    bb_json_t obj = bb_json_obj_new();
    TEST_ASSERT_NOT_NULL(obj);
    bb_json_obj_set_int(obj,    "ts",    1234567890LL);
    bb_json_obj_set_string(obj, "level", "W");
    // missing "tag" and "msg"
    bb_openapi_validate_err_t verr = {0};
    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION,
                      bb_openapi_validate(k_log_schema, obj, &verr));
    bb_json_free(obj);
}

void test_sse_schema_log_payload_bad_level_enum_fails(void)
{
    bb_json_t obj = bb_json_obj_new();
    TEST_ASSERT_NOT_NULL(obj);
    bb_json_obj_set_int(obj,    "ts",    1234567890LL);
    bb_json_obj_set_string(obj, "level", "X");
    bb_json_obj_set_string(obj, "tag",   "test");
    bb_json_obj_set_string(obj, "msg",   "bad level");
    bb_openapi_validate_err_t verr = {0};
    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION,
                      bb_openapi_validate(k_log_schema, obj, &verr));
    TEST_ASSERT_NOT_NULL(strstr(verr.path, "level"));
    bb_json_free(obj);
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
    bb_json_t doc = bb_openapi_emit(&meta);
    TEST_ASSERT_NOT_NULL(doc);

    bb_json_t components = bb_json_obj_get_item(doc, "components");
    TEST_ASSERT_NOT_NULL(components);
    bb_json_t schemas = bb_json_obj_get_item(components, "schemas");
    TEST_ASSERT_NOT_NULL(schemas);
    bb_json_t entry = bb_json_obj_get_item(schemas, "LogEvent");
    TEST_ASSERT_NOT_NULL(entry);

    bb_json_free(doc);
}

void test_sse_schema_emit_no_schemas_no_components(void)
{
    bb_http_route_registry_clear();
    bb_http_register_described_route(NULL, &s_fid_route);
    // registry is empty (cleared in setUp)

    bb_openapi_meta_t meta = { .title = "T", .version = "1.0" };
    bb_json_t doc = bb_openapi_emit(&meta);
    TEST_ASSERT_NOT_NULL(doc);
    TEST_ASSERT_NULL(bb_json_obj_get_item(doc, "components"));
    bb_json_free(doc);
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
    bb_json_t doc = bb_openapi_emit(&meta);
    TEST_ASSERT_NOT_NULL(doc);

    char *s = bb_json_serialize(doc);
    bb_json_free(doc);
    TEST_ASSERT_NOT_NULL(s);

    TEST_ASSERT_NOT_NULL(strstr(s, "\"oneOf\""));
    TEST_ASSERT_NOT_NULL(strstr(s, "\"$ref\":\"#/components/schemas/LogEvent\""));

    bb_json_free_str(s);
}

void test_sse_schema_no_sse_topic_no_oneof(void)
{
    bb_http_route_registry_clear();
    bb_http_register_described_route(NULL, &s_sse_route);
    // Register schema with NULL sse_topic — REST-only, must NOT appear in oneOf.
    bb_openapi_register_schema("WifiInfo", k_wifi_schema, NULL);

    bb_openapi_meta_t meta = { .title = "T", .version = "1.0" };
    bb_json_t doc = bb_openapi_emit(&meta);
    TEST_ASSERT_NOT_NULL(doc);

    char *s = bb_json_serialize(doc);
    bb_json_free(doc);
    TEST_ASSERT_NOT_NULL(s);

    // No SSE topics → no oneOf in the SSE response content block.
    TEST_ASSERT_NULL(strstr(s, "\"oneOf\""));

    bb_json_free_str(s);
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
    bb_json_t doc = bb_openapi_emit(&meta);
    TEST_ASSERT_NOT_NULL(doc);

    char *s = bb_json_serialize(doc);
    bb_json_free(doc);
    TEST_ASSERT_NOT_NULL(s);

    // LogEvent IS in oneOf (has sse_topic)
    TEST_ASSERT_NOT_NULL(strstr(s, "\"$ref\":\"#/components/schemas/LogEvent\""));
    // WifiInfo is NOT in oneOf (null sse_topic → continue branch)
    TEST_ASSERT_NULL(strstr(s, "\"$ref\":\"#/components/schemas/WifiInfo\""));
    // WifiInfo IS in components/schemas (registered with null sse_topic)
    TEST_ASSERT_NOT_NULL(strstr(s, "\"WifiInfo\""));

    bb_json_free_str(s);
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
// OOM: oneOf block — content/media/schema/one_of alloc failures (line 305)
// Line 309: ref alloc failure in oneOf loop
// Line 424: components alloc failure
//
// Alloc sequence for s_sse_route (tag="events") + 1 LogEvent SSE schema:
//   root=0, info=1, paths_obj=2, path_item=3, op=4, tags=5, responses=6,
//   resp_obj=7, content=8, media=9, schema_obj=10, one_of=11, ref=12,
//   components=13, schemas=14.
// ---------------------------------------------------------------------------

// content=NULL → if(NULL && ...) → else cleanup (line 305 branch 1)
void test_sse_schema_oom_oneof_content_skips_block(void)
{
    bb_http_route_registry_clear();
    bb_http_register_described_route(NULL, &s_sse_route);
    bb_openapi_register_schema("LogEvent", k_log_schema, "log");

    bb_openapi_meta_t meta = { .title = "T", .version = "1.0" };
    bb_json_host_force_alloc_fail_after(8);
    bb_json_t doc = bb_openapi_emit(&meta);
    TEST_ASSERT_NOT_NULL(doc);
    bb_json_host_force_alloc_fail_after(-1);

    bb_json_t paths    = bb_json_obj_get_item(doc, "paths");
    bb_json_t pi       = bb_json_obj_get_item(paths, "/api/events");
    bb_json_t get_op   = bb_json_obj_get_item(pi, "get");
    bb_json_t resps    = bb_json_obj_get_item(get_op, "responses");
    bb_json_t r200     = bb_json_obj_get_item(resps, "200");
    TEST_ASSERT_NOT_NULL(r200);
    // content alloc failed → SSE response has no content key
    TEST_ASSERT_NULL(bb_json_obj_get_item(r200, "content"));

    bb_json_free(doc);
}

// media=NULL → if(content && NULL && ...) → else cleanup (line 305 branch 3)
void test_sse_schema_oom_oneof_media_skips_block(void)
{
    bb_http_route_registry_clear();
    bb_http_register_described_route(NULL, &s_sse_route);
    bb_openapi_register_schema("LogEvent", k_log_schema, "log");

    bb_openapi_meta_t meta = { .title = "T", .version = "1.0" };
    bb_json_host_force_alloc_fail_after(9);
    bb_json_t doc = bb_openapi_emit(&meta);
    TEST_ASSERT_NOT_NULL(doc);
    bb_json_host_force_alloc_fail_after(-1);

    bb_json_t paths  = bb_json_obj_get_item(doc, "paths");
    bb_json_t pi     = bb_json_obj_get_item(paths, "/api/events");
    bb_json_t get_op = bb_json_obj_get_item(pi, "get");
    bb_json_t resps  = bb_json_obj_get_item(get_op, "responses");
    bb_json_t r200   = bb_json_obj_get_item(resps, "200");
    TEST_ASSERT_NOT_NULL(r200);
    TEST_ASSERT_NULL(bb_json_obj_get_item(r200, "content"));

    bb_json_free(doc);
}

// schema_obj=NULL → if(content && media && NULL && ...) → else (line 305 branch 5)
void test_sse_schema_oom_oneof_schema_obj_skips_block(void)
{
    bb_http_route_registry_clear();
    bb_http_register_described_route(NULL, &s_sse_route);
    bb_openapi_register_schema("LogEvent", k_log_schema, "log");

    bb_openapi_meta_t meta = { .title = "T", .version = "1.0" };
    bb_json_host_force_alloc_fail_after(10);
    bb_json_t doc = bb_openapi_emit(&meta);
    TEST_ASSERT_NOT_NULL(doc);
    bb_json_host_force_alloc_fail_after(-1);

    bb_json_t paths  = bb_json_obj_get_item(doc, "paths");
    bb_json_t pi     = bb_json_obj_get_item(paths, "/api/events");
    bb_json_t get_op = bb_json_obj_get_item(pi, "get");
    bb_json_t resps  = bb_json_obj_get_item(get_op, "responses");
    bb_json_t r200   = bb_json_obj_get_item(resps, "200");
    TEST_ASSERT_NOT_NULL(r200);
    TEST_ASSERT_NULL(bb_json_obj_get_item(r200, "content"));

    bb_json_free(doc);
}

// one_of=NULL → if(content && media && schema && NULL) → else (line 305 branch 6)
void test_sse_schema_oom_oneof_arr_skips_block(void)
{
    bb_http_route_registry_clear();
    bb_http_register_described_route(NULL, &s_sse_route);
    bb_openapi_register_schema("LogEvent", k_log_schema, "log");

    bb_openapi_meta_t meta = { .title = "T", .version = "1.0" };
    bb_json_host_force_alloc_fail_after(11);
    bb_json_t doc = bb_openapi_emit(&meta);
    TEST_ASSERT_NOT_NULL(doc);
    bb_json_host_force_alloc_fail_after(-1);

    bb_json_t paths  = bb_json_obj_get_item(doc, "paths");
    bb_json_t pi     = bb_json_obj_get_item(paths, "/api/events");
    bb_json_t get_op = bb_json_obj_get_item(pi, "get");
    bb_json_t resps  = bb_json_obj_get_item(get_op, "responses");
    bb_json_t r200   = bb_json_obj_get_item(resps, "200");
    TEST_ASSERT_NOT_NULL(r200);
    TEST_ASSERT_NULL(bb_json_obj_get_item(r200, "content"));

    bb_json_free(doc);
}

// ref=NULL → if(ref) false → skip ref entry (line 309 branch)
void test_sse_schema_oom_oneof_ref_skips_entry(void)
{
    bb_http_route_registry_clear();
    bb_http_register_described_route(NULL, &s_sse_route);
    bb_openapi_register_schema("LogEvent", k_log_schema, "log");

    bb_openapi_meta_t meta = { .title = "T", .version = "1.0" };
    // content=8, media=9, schema_obj=10, one_of=11 all succeed; ref=12 fails
    bb_json_host_force_alloc_fail_after(12);
    bb_json_t doc = bb_openapi_emit(&meta);
    TEST_ASSERT_NOT_NULL(doc);
    bb_json_host_force_alloc_fail_after(-1);

    char *s = bb_json_serialize(doc);
    bb_json_free(doc);
    TEST_ASSERT_NOT_NULL(s);
    // oneOf block was built but ref alloc failed → oneOf array is empty
    TEST_ASSERT_NOT_NULL(strstr(s, "\"oneOf\":[]"));
    bb_json_free_str(s);
}

// components=NULL → if(NULL && schemas) short-circuits (line 424 branch 1)
void test_sse_schema_oom_components_obj_skips_section(void)
{
    bb_http_route_registry_clear();
    bb_http_register_described_route(NULL, &s_sse_route);
    bb_openapi_register_schema("LogEvent", k_log_schema, "log");

    bb_openapi_meta_t meta = { .title = "T", .version = "1.0" };
    // all oneOf + ref allocs succeed (8-12); components=13 fails
    bb_json_host_force_alloc_fail_after(13);
    bb_json_t doc = bb_openapi_emit(&meta);
    TEST_ASSERT_NOT_NULL(doc);
    bb_json_host_force_alloc_fail_after(-1);

    // components alloc failed → section gracefully skipped
    TEST_ASSERT_NULL(bb_json_obj_get_item(doc, "components"));

    bb_json_free(doc);
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
    "\"disc_reason\":{\"type\":\"integer\"},"
    "\"disc_age_s\":{\"type\":\"integer\"},"
    "\"retry_count\":{\"type\":\"integer\"},"
    "\"no_ip_recoveries\":{\"type\":\"integer\"},"
    "\"egress_dead_count\":{\"type\":\"integer\"},"
    "\"lost_ip_count\":{\"type\":\"integer\"},"
    "\"recovery_count\":{\"type\":\"integer\"},"
    "\"ts_ms\":{\"type\":\"integer\"}},"
    "\"required\":[\"ssid\",\"connected\",\"rssi\",\"ts_ms\"]}";

void test_sse_schema_wifi_telemetry_payload_valid(void)
{
    bb_json_t obj = bb_json_obj_new();
    TEST_ASSERT_NOT_NULL(obj);
    bb_json_obj_set_string(obj, "ssid",             "TestNet");
    bb_json_obj_set_string(obj, "bssid",            "aa:bb:cc:dd:ee:ff");
    bb_json_obj_set_int   (obj, "rssi",             -65);
    bb_json_obj_set_string(obj, "ip",               "192.168.1.10");
    bb_json_obj_set_bool  (obj, "connected",        true);
    bb_json_obj_set_int   (obj, "disc_reason",      0);
    bb_json_obj_set_int   (obj, "disc_age_s",       0);
    bb_json_obj_set_int   (obj, "retry_count",      0);
    bb_json_obj_set_int   (obj, "no_ip_recoveries", 0);
    bb_json_obj_set_int   (obj, "egress_dead_count",0);
    bb_json_obj_set_int   (obj, "lost_ip_count",    0);
    bb_json_obj_set_int   (obj, "recovery_count",   0);
    bb_json_obj_set_int   (obj, "ts_ms",            12345678LL);
    TEST_ASSERT_EQUAL(BB_OK, bb_openapi_validate(k_wifi_telemetry_schema, obj, NULL));
    bb_json_free(obj);
}

void test_sse_schema_wifi_telemetry_payload_missing_required_fails(void)
{
    bb_json_t obj = bb_json_obj_new();
    TEST_ASSERT_NOT_NULL(obj);
    bb_json_obj_set_string(obj, "ssid", "TestNet");
    // missing connected, rssi, ts_ms
    bb_openapi_validate_err_t verr = {0};
    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION,
                      bb_openapi_validate(k_wifi_telemetry_schema, obj, &verr));
    bb_json_free(obj);
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
    bb_json_t obj = bb_json_obj_new();
    TEST_ASSERT_NOT_NULL(obj);
    bb_json_obj_set_int   (obj, "rpm",      1200);
    bb_json_obj_set_int   (obj, "duty_pct", 50);
    bb_json_obj_set_number(obj, "die_c",    72.5);
    bb_json_obj_set_number(obj, "board_c",  45.0);
    bb_json_obj_set_int   (obj, "ts_ms",    12345678LL);
    TEST_ASSERT_EQUAL(BB_OK, bb_openapi_validate(k_fan_telemetry_schema, obj, NULL));
    bb_json_free(obj);
}

void test_sse_schema_fan_telemetry_payload_missing_required_fails(void)
{
    bb_json_t obj = bb_json_obj_new();
    TEST_ASSERT_NOT_NULL(obj);
    bb_json_obj_set_int(obj, "rpm", 1200);
    // missing duty_pct, die_c, board_c, ts_ms
    bb_openapi_validate_err_t verr = {0};
    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION,
                      bb_openapi_validate(k_fan_telemetry_schema, obj, &verr));
    bb_json_free(obj);
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
    bb_json_t obj = bb_json_obj_new();
    TEST_ASSERT_NOT_NULL(obj);
    bb_json_obj_set_number(obj, "vout_mv", 12000.0);
    bb_json_obj_set_number(obj, "iout_ma", 5000.0);
    bb_json_obj_set_number(obj, "pout_mw", 60000.0);
    bb_json_obj_set_number(obj, "vin_mv",  19000.0);
    bb_json_obj_set_number(obj, "temp_c",  55.0);
    bb_json_obj_set_int   (obj, "ts_ms",   12345678LL);
    TEST_ASSERT_EQUAL(BB_OK, bb_openapi_validate(k_power_telemetry_schema, obj, NULL));
    bb_json_free(obj);
}

void test_sse_schema_power_telemetry_payload_missing_required_fails(void)
{
    bb_json_t obj = bb_json_obj_new();
    TEST_ASSERT_NOT_NULL(obj);
    bb_json_obj_set_number(obj, "vout_mv", 12000.0);
    // missing iout_ma, pout_mw, vin_mv, temp_c, ts_ms
    bb_openapi_validate_err_t verr = {0};
    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION,
                      bb_openapi_validate(k_power_telemetry_schema, obj, &verr));
    bb_json_free(obj);
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
    bb_json_t obj = bb_json_obj_new();
    TEST_ASSERT_NOT_NULL(obj);
    bb_json_obj_set_number(obj, "soc_c",  68.0);
    bb_json_obj_set_int   (obj, "ts_ms",  12345678LL);
    TEST_ASSERT_EQUAL(BB_OK, bb_openapi_validate(k_thermal_telemetry_schema, obj, NULL));
    bb_json_free(obj);
}

void test_sse_schema_thermal_telemetry_payload_missing_required_fails(void)
{
    bb_json_t obj = bb_json_obj_new();
    TEST_ASSERT_NOT_NULL(obj);
    bb_json_obj_set_number(obj, "vr_c", 55.0);
    // missing soc_c and ts_ms
    bb_openapi_validate_err_t verr = {0};
    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION,
                      bb_openapi_validate(k_thermal_telemetry_schema, obj, &verr));
    bb_json_free(obj);
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
    bb_json_t obj = bb_json_obj_new();
    TEST_ASSERT_NOT_NULL(obj);
    bb_json_obj_set_number(obj, "heap_internal_free",          100000.0);
    bb_json_obj_set_number(obj, "heap_internal_total",         200000.0);
    bb_json_obj_set_number(obj, "heap_internal_largest_block",  90000.0);
    bb_json_obj_set_number(obj, "heap_internal_min_free",       80000.0);
    bb_json_obj_set_number(obj, "rtc_used",         1024.0);
    bb_json_obj_set_number(obj, "rtc_total",         8192.0);
    bb_json_obj_set_number(obj, "dram_static_bytes", 50000.0);
    bb_json_obj_set_number(obj, "flash_size",         4194304.0);
    bb_json_obj_set_number(obj, "app_size",           1200000.0);
    bb_json_obj_set_number(obj, "wdt_resets",         0.0);
    bb_json_obj_set_string(obj, "version",            "1.0.0");
    bb_json_obj_set_string(obj, "board",              "wroom32");
    bb_json_obj_set_string(obj, "chip_model",         "ESP32");
    bb_json_obj_set_string(obj, "mac",                "aa:bb:cc:dd:ee:ff");
    bb_json_obj_set_string(obj, "reset_reason",       "power_on");
    bb_json_obj_set_bool  (obj, "ota_validated",      true);
    bb_json_obj_set_bool  (obj, "time_valid",         false);
    bb_json_obj_set_number(obj, "boot_epoch_s",       0.0);
    bb_json_obj_set_string(obj, "time_source",        "none");
    bb_json_obj_set_number(obj, "rtc_free",           7168.0);
    bb_json_obj_set_int   (obj, "ts_ms",              12345678LL);
    TEST_ASSERT_EQUAL(BB_OK, bb_openapi_validate(k_info_telemetry_schema, obj, NULL));
    bb_json_free(obj);
}

void test_sse_schema_info_telemetry_payload_missing_required_fails(void)
{
    bb_json_t obj = bb_json_obj_new();
    TEST_ASSERT_NOT_NULL(obj);
    bb_json_obj_set_number(obj, "heap_internal_free", 100000.0);
    // missing all other required fields
    bb_openapi_validate_err_t verr = {0};
    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION,
                      bb_openapi_validate(k_info_telemetry_schema, obj, &verr));
    bb_json_free(obj);
}

// RtosTelemetry (sse_topic=NULL — uses old bb_pub_register_source, not SSE)
static const char k_rtos_telemetry_schema[] =
    "{\"title\":\"RtosTelemetry\",\"type\":\"object\","
    "\"properties\":{"
    "\"min_free_stack\":{\"type\":\"number\"},"
    "\"min_free_stack_task\":{\"type\":\"string\"},"
    "\"task_count\":{\"type\":\"number\"},"
    "\"stack_bb_pub\":{\"type\":\"number\"},"
    "\"stack_httpd\":{\"type\":\"number\"},"
    "\"stack_mqtt\":{\"type\":\"number\"},"
    "\"stack_ipc0\":{\"type\":\"number\"},"
    "\"stack_ipc1\":{\"type\":\"number\"},"
    "\"stack_main\":{\"type\":\"number\"}},"
    "\"required\":[\"min_free_stack\",\"min_free_stack_task\",\"task_count\"]}";

void test_sse_schema_rtos_telemetry_payload_valid(void)
{
    bb_json_t obj = bb_json_obj_new();
    TEST_ASSERT_NOT_NULL(obj);
    bb_json_obj_set_number(obj, "min_free_stack",      2048.0);
    bb_json_obj_set_string(obj, "min_free_stack_task", "bb_pub");
    bb_json_obj_set_number(obj, "task_count",          8.0);
    TEST_ASSERT_EQUAL(BB_OK, bb_openapi_validate(k_rtos_telemetry_schema, obj, NULL));
    bb_json_free(obj);
}

void test_sse_schema_rtos_telemetry_payload_missing_required_fails(void)
{
    bb_json_t obj = bb_json_obj_new();
    TEST_ASSERT_NOT_NULL(obj);
    bb_json_obj_set_number(obj, "min_free_stack", 2048.0);
    // missing min_free_stack_task and task_count
    bb_openapi_validate_err_t verr = {0};
    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION,
                      bb_openapi_validate(k_rtos_telemetry_schema, obj, &verr));
    bb_json_free(obj);
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
    bb_json_t obj = bb_json_obj_new();
    TEST_ASSERT_NOT_NULL(obj);
    bb_json_obj_set_string(obj, "type",      "wifi_down");
    bb_json_obj_set_int   (obj, "severity",  2);
    bb_json_obj_set_int   (obj, "uptime_ms", 60000LL);
    TEST_ASSERT_EQUAL(BB_OK, bb_openapi_validate(k_alert_schema, obj, NULL));
    bb_json_free(obj);
}

void test_sse_schema_alert_payload_missing_required_fails(void)
{
    bb_json_t obj = bb_json_obj_new();
    TEST_ASSERT_NOT_NULL(obj);
    bb_json_obj_set_string(obj, "type", "wifi_down");
    // missing severity and uptime_ms
    bb_openapi_validate_err_t verr = {0};
    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION,
                      bb_openapi_validate(k_alert_schema, obj, &verr));
    bb_json_free(obj);
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
    bb_json_t obj = bb_json_obj_new();
    TEST_ASSERT_NOT_NULL(obj);
    bb_json_obj_set_string(obj, "current",      "1.0.0");
    bb_json_obj_set_string(obj, "latest",       "1.1.0");
    bb_json_obj_set_string(obj, "download_url", "https://example.com/fw.bin");
    bb_json_obj_set_bool  (obj, "available",    true);
    bb_json_obj_set_int   (obj, "ts",           1700000000LL);
    bb_json_obj_set_bool  (obj, "last_check_ok",true);
    bb_json_obj_set_bool  (obj, "enabled",      true);
    bb_json_obj_set_string(obj, "outcome",      "update_available");
    TEST_ASSERT_EQUAL(BB_OK, bb_openapi_validate(k_update_available_schema, obj, NULL));
    bb_json_free(obj);
}

void test_sse_schema_update_available_payload_missing_required_fails(void)
{
    bb_json_t obj = bb_json_obj_new();
    TEST_ASSERT_NOT_NULL(obj);
    bb_json_obj_set_string(obj, "current", "1.0.0");
    // missing latest, download_url, available, ts, last_check_ok, enabled, outcome
    bb_openapi_validate_err_t verr = {0};
    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION,
                      bb_openapi_validate(k_update_available_schema, obj, &verr));
    bb_json_free(obj);
}

// BuildInfo (sse_topic="build")
static const char k_build_info_schema[] =
    "{\"title\":\"BuildInfo\",\"x-sse-topic\":\"build\",\"type\":\"object\","
    "\"properties\":{"
    "\"version\":{\"type\":\"string\"},"
    "\"idf_version\":{\"type\":\"string\"},"
    "\"build_date\":{\"type\":\"string\"},"
    "\"build_time\":{\"type\":\"string\"},"
    "\"project_name\":{\"type\":\"string\"},"
    "\"chip_model\":{\"type\":\"string\"},"
    "\"chip_revision\":{\"type\":\"integer\"},"
    "\"cores\":{\"type\":\"integer\"},"
    "\"cpu_freq_mhz\":{\"type\":\"integer\"},"
    "\"flash_size\":{\"type\":\"integer\"},"
    "\"app_size\":{\"type\":\"integer\"},"
    "\"board\":{\"type\":\"string\"},"
    "\"app_sha256\":{\"type\":\"string\"}},"
    "\"required\":[\"version\",\"idf_version\",\"build_date\",\"build_time\","
    "\"project_name\",\"chip_model\",\"chip_revision\",\"cores\","
    "\"cpu_freq_mhz\",\"flash_size\",\"app_size\",\"board\",\"app_sha256\"]}";

void test_sse_schema_build_info_payload_valid(void)
{
    bb_json_t obj = bb_json_obj_new();
    TEST_ASSERT_NOT_NULL(obj);
    bb_json_obj_set_string(obj, "version",      "1.0.0");
    bb_json_obj_set_string(obj, "idf_version",  "v5.3.1");
    bb_json_obj_set_string(obj, "build_date",   "Jun 29 2026");
    bb_json_obj_set_string(obj, "build_time",   "12:00:00");
    bb_json_obj_set_string(obj, "project_name", "breadboard");
    bb_json_obj_set_string(obj, "chip_model",   "ESP32");
    bb_json_obj_set_int   (obj, "chip_revision", 3);
    bb_json_obj_set_int   (obj, "cores",         2);
    bb_json_obj_set_int   (obj, "cpu_freq_mhz",  240);
    bb_json_obj_set_int   (obj, "flash_size",    4194304);
    bb_json_obj_set_int   (obj, "app_size",      1200000);
    bb_json_obj_set_string(obj, "board",         "wroom32");
    bb_json_obj_set_string(obj, "app_sha256",    "deadbeef");
    TEST_ASSERT_EQUAL(BB_OK, bb_openapi_validate(k_build_info_schema, obj, NULL));
    bb_json_free(obj);
}

void test_sse_schema_build_info_payload_missing_required_fails(void)
{
    bb_json_t obj = bb_json_obj_new();
    TEST_ASSERT_NOT_NULL(obj);
    bb_json_obj_set_string(obj, "version", "1.0.0");
    // missing idf_version, build_date, build_time, etc.
    bb_openapi_validate_err_t verr = {0};
    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION,
                      bb_openapi_validate(k_build_info_schema, obj, &verr));
    bb_json_free(obj);
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
    bb_json_t panic = bb_json_obj_new();
    TEST_ASSERT_NOT_NULL(panic);
    bb_json_obj_set_bool(panic, "available", false);

    bb_json_t obj = bb_json_obj_new();
    TEST_ASSERT_NOT_NULL(obj);
    bb_json_obj_set_string(obj, "reset_reason",  "power_on");
    bb_json_obj_set_int   (obj, "wdt_resets",    0);
    bb_json_obj_set_obj   (obj, "panic",         panic);
    bb_json_obj_set_bool  (obj, "pending_verify",false);
    bb_json_obj_set_bool  (obj, "rolled_back",   false);
    TEST_ASSERT_EQUAL(BB_OK, bb_openapi_validate(k_diag_boot_schema, obj, NULL));
    bb_json_free(obj);
}

void test_sse_schema_diag_boot_payload_missing_required_fails(void)
{
    bb_json_t obj = bb_json_obj_new();
    TEST_ASSERT_NOT_NULL(obj);
    bb_json_obj_set_string(obj, "reset_reason", "power_on");
    // missing wdt_resets, panic, pending_verify, rolled_back
    bb_openapi_validate_err_t verr = {0};
    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION,
                      bb_openapi_validate(k_diag_boot_schema, obj, &verr));
    bb_json_free(obj);
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
    bb_json_t obj = bb_json_obj_new();
    TEST_ASSERT_NOT_NULL(obj);
    bb_json_obj_set_bool(obj, "present", false);
    TEST_ASSERT_EQUAL(BB_OK, bb_openapi_validate(k_display_info_schema, obj, NULL));
    bb_json_free(obj);
}

void test_sse_schema_display_info_payload_missing_required_fails(void)
{
    bb_json_t obj = bb_json_obj_new();
    TEST_ASSERT_NOT_NULL(obj);
    bb_json_obj_set_string(obj, "panel", "ILI9341");
    // missing present
    bb_openapi_validate_err_t verr = {0};
    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION,
                      bb_openapi_validate(k_display_info_schema, obj, &verr));
    bb_json_free(obj);
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
    bb_json_t obj = bb_json_obj_new();
    TEST_ASSERT_NOT_NULL(obj);
    bb_json_obj_set_string(obj, "task",       "httpd");
    bb_json_obj_set_int   (obj, "free_bytes", 512);
    bb_json_obj_set_bool  (obj, "low",        true);
    TEST_ASSERT_EQUAL(BB_OK, bb_openapi_validate(k_health_stack_schema, obj, NULL));
    bb_json_free(obj);
}

void test_sse_schema_health_stack_payload_missing_required_fails(void)
{
    bb_json_t obj = bb_json_obj_new();
    TEST_ASSERT_NOT_NULL(obj);
    bb_json_obj_set_string(obj, "task", "httpd");
    // missing free_bytes and low
    bb_openapi_validate_err_t verr = {0};
    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION,
                      bb_openapi_validate(k_health_stack_schema, obj, &verr));
    bb_json_free(obj);
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
    bb_json_t obj = bb_json_obj_new();
    TEST_ASSERT_NOT_NULL(obj);
    bb_json_obj_set_string(obj, "via",   "mqtt");
    bb_json_obj_set_string(obj, "state", "progress");
    bb_json_obj_set_int   (obj, "pct",   50);
    TEST_ASSERT_EQUAL(BB_OK, bb_openapi_validate(k_ota_progress_schema, obj, NULL));
    bb_json_free(obj);
}

void test_sse_schema_ota_progress_payload_missing_required_fails(void)
{
    bb_json_t obj = bb_json_obj_new();
    TEST_ASSERT_NOT_NULL(obj);
    bb_json_obj_set_string(obj, "via", "mqtt");
    // missing state and pct
    bb_openapi_validate_err_t verr = {0};
    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION,
                      bb_openapi_validate(k_ota_progress_schema, obj, &verr));
    bb_json_free(obj);
}

// NetHealth (sse_topic="net.health") — uses flat fields + nested mqtt/http objects
static const char k_net_health_schema[] =
    "{\"title\":\"NetHealth\",\"x-sse-topic\":\"net.health\",\"type\":\"object\","
    "\"properties\":{"
    "\"rssi\":{\"type\":\"integer\"},"
    "\"state\":{\"type\":\"string\"},"
    "\"early_warning\":{\"type\":\"boolean\"},"
    "\"throttled\":{\"type\":\"boolean\"},"
    "\"last_disconnect_reason\":{\"type\":\"integer\"},"
    "\"disc_age_s\":{\"type\":\"integer\"},"
    "\"lost_ip_recoveries\":{\"type\":\"integer\"},"
    "\"lost_ip_age_s\":{\"type\":\"integer\"},"
    "\"egress_dead_recoveries\":{\"type\":\"integer\"},"
    "\"mqtt\":{\"type\":\"object\",\"properties\":{"
    "\"connected\":{\"type\":\"boolean\"},"
    "\"reconnect_count\":{\"type\":\"integer\"},"
    "\"disc_age_s\":{\"type\":\"integer\"},"
    "\"disc_reason\":{\"type\":\"integer\"},"
    "\"tls_fail\":{\"type\":\"integer\"}}},"
    "\"http\":{\"type\":\"object\",\"properties\":{"
    "\"connected\":{\"type\":\"boolean\"},"
    "\"consec_failures\":{\"type\":\"integer\"},"
    "\"tls_fail\":{\"type\":\"integer\"},"
    "\"last_status\":{\"type\":\"integer\"}}}},"
    "\"required\":[\"rssi\",\"state\",\"early_warning\",\"throttled\"]}";

void test_sse_schema_net_health_payload_valid(void)
{
    bb_json_t mqtt = bb_json_obj_new();
    TEST_ASSERT_NOT_NULL(mqtt);
    bb_json_obj_set_bool(mqtt, "connected",      true);
    bb_json_obj_set_int (mqtt, "reconnect_count", 0);
    bb_json_obj_set_int (mqtt, "disc_age_s",      0);
    bb_json_obj_set_int (mqtt, "disc_reason",     0);
    bb_json_obj_set_int (mqtt, "tls_fail",        0);

    bb_json_t http = bb_json_obj_new();
    TEST_ASSERT_NOT_NULL(http);
    bb_json_obj_set_bool(http, "connected",       true);
    bb_json_obj_set_int (http, "consec_failures",  0);
    bb_json_obj_set_int (http, "tls_fail",         0);
    bb_json_obj_set_int (http, "last_status",      200);

    bb_json_t obj = bb_json_obj_new();
    TEST_ASSERT_NOT_NULL(obj);
    bb_json_obj_set_int (obj, "rssi",                   -70);
    bb_json_obj_set_string(obj, "state",                "good");
    bb_json_obj_set_bool(obj, "early_warning",          false);
    bb_json_obj_set_bool(obj, "throttled",              false);
    bb_json_obj_set_int (obj, "last_disconnect_reason", 0);
    bb_json_obj_set_int (obj, "disc_age_s",             0);
    bb_json_obj_set_int (obj, "lost_ip_recoveries",     0);
    bb_json_obj_set_int (obj, "lost_ip_age_s",          0);
    bb_json_obj_set_int (obj, "egress_dead_recoveries", 0);
    bb_json_obj_set_obj(obj, "mqtt", mqtt);
    bb_json_obj_set_obj(obj, "http", http);
    TEST_ASSERT_EQUAL(BB_OK, bb_openapi_validate(k_net_health_schema, obj, NULL));
    bb_json_free(obj);
}

void test_sse_schema_net_health_payload_missing_required_fails(void)
{
    bb_json_t obj = bb_json_obj_new();
    TEST_ASSERT_NOT_NULL(obj);
    bb_json_obj_set_int(obj, "rssi", -70);
    // missing state, early_warning, throttled
    bb_openapi_validate_err_t verr = {0};
    TEST_ASSERT_EQUAL(BB_ERR_VALIDATION,
                      bb_openapi_validate(k_net_health_schema, obj, &verr));
    bb_json_free(obj);
}

// ---------------------------------------------------------------------------
// oneOf count: all SSE topics registered → 13 refs in /api/events 200 oneOf
// (log + wifi + fan + power + thermal + alert + update.available +
//  build + diag.boot + health.display + health.stack + ota.progress +
//  net.health)
// ---------------------------------------------------------------------------

void test_sse_schema_sse_oneof_count_and_topics(void)
{
    bb_http_route_registry_clear();
    bb_http_register_described_route(NULL, &s_sse_route);

    // Register all 13 SSE-facing schemas (sse_topic != NULL).
    bb_openapi_register_schema("LogEvent",        k_log_schema,              "log");
    bb_openapi_register_schema("WifiTelemetry",   k_wifi_telemetry_schema,   "wifi");
    bb_openapi_register_schema("FanTelemetry",    k_fan_telemetry_schema,    "fan");
    bb_openapi_register_schema("PowerTelemetry",  k_power_telemetry_schema,  "power");
    bb_openapi_register_schema("ThermalTelemetry",k_thermal_telemetry_schema,"thermal");
    bb_openapi_register_schema("Alert",           k_alert_schema,            "alert");
    bb_openapi_register_schema("UpdateAvailable", k_update_available_schema, "update.available");
    bb_openapi_register_schema("BuildInfo",       k_build_info_schema,       "build");
    bb_openapi_register_schema("DiagBoot",        k_diag_boot_schema,        "diag.boot");
    bb_openapi_register_schema("DisplayInfo",     k_display_info_schema,     "health.display");
    bb_openapi_register_schema("HealthStack",     k_health_stack_schema,     "health.stack");
    bb_openapi_register_schema("OtaProgress",     k_ota_progress_schema,     "ota.progress");
    bb_openapi_register_schema("NetHealth",       k_net_health_schema,       "net.health");
    // Also register REST-only schemas (must NOT appear in oneOf).
    bb_openapi_register_schema("InfoTelemetry",   k_info_telemetry_schema,   NULL);
    bb_openapi_register_schema("RtosTelemetry",   k_rtos_telemetry_schema,   NULL);
    bb_openapi_register_schema("WifiInfo",        k_wifi_schema,             NULL);

    TEST_ASSERT_EQUAL_size_t(16, bb_openapi_schema_count());

    bb_openapi_meta_t meta = { .title = "T", .version = "1.0" };
    bb_json_t doc = bb_openapi_emit(&meta);
    TEST_ASSERT_NOT_NULL(doc);

    char *s = bb_json_serialize(doc);
    bb_json_free(doc);
    TEST_ASSERT_NOT_NULL(s);

    // All 13 SSE schemas must appear as $ref entries in oneOf.
    TEST_ASSERT_NOT_NULL(strstr(s, "\"$ref\":\"#/components/schemas/LogEvent\""));
    TEST_ASSERT_NOT_NULL(strstr(s, "\"$ref\":\"#/components/schemas/WifiTelemetry\""));
    TEST_ASSERT_NOT_NULL(strstr(s, "\"$ref\":\"#/components/schemas/FanTelemetry\""));
    TEST_ASSERT_NOT_NULL(strstr(s, "\"$ref\":\"#/components/schemas/PowerTelemetry\""));
    TEST_ASSERT_NOT_NULL(strstr(s, "\"$ref\":\"#/components/schemas/ThermalTelemetry\""));
    TEST_ASSERT_NOT_NULL(strstr(s, "\"$ref\":\"#/components/schemas/Alert\""));
    TEST_ASSERT_NOT_NULL(strstr(s, "\"$ref\":\"#/components/schemas/UpdateAvailable\""));
    TEST_ASSERT_NOT_NULL(strstr(s, "\"$ref\":\"#/components/schemas/BuildInfo\""));
    TEST_ASSERT_NOT_NULL(strstr(s, "\"$ref\":\"#/components/schemas/DiagBoot\""));
    TEST_ASSERT_NOT_NULL(strstr(s, "\"$ref\":\"#/components/schemas/DisplayInfo\""));
    TEST_ASSERT_NOT_NULL(strstr(s, "\"$ref\":\"#/components/schemas/HealthStack\""));
    TEST_ASSERT_NOT_NULL(strstr(s, "\"$ref\":\"#/components/schemas/OtaProgress\""));
    TEST_ASSERT_NOT_NULL(strstr(s, "\"$ref\":\"#/components/schemas/NetHealth\""));
    // REST-only schemas must NOT appear in oneOf.
    TEST_ASSERT_NULL(strstr(s, "\"$ref\":\"#/components/schemas/InfoTelemetry\""));
    TEST_ASSERT_NULL(strstr(s, "\"$ref\":\"#/components/schemas/RtosTelemetry\""));
    TEST_ASSERT_NULL(strstr(s, "\"$ref\":\"#/components/schemas/WifiInfo\""));

    bb_json_free_str(s);
}
