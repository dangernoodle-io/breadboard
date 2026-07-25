// NOTE: this file intentionally still references bb_json_* (bb_json.h,
// bb_json_test_hooks.h) for a handful of deliberately-retained tests --
// B1-1054 PR 5 (this port). platform/host/bb_openapi/bb_openapi_emit_tree.c
// (the TREE emitter) is still present until B1-1054 PR 6 deletes it, so a
// test that is the SOLE cover for one of its lines/branches must keep
// driving bb_openapi_emit() (tree) rather than move to the stream path --
// same rule PR 4 applied to test_openapi_emit.c's 19 OOM tests. Retained
// here, each commented at its call site:
//   - the 3 OOM tests (content/media/components alloc-failure) -- sole
//     cover for the tree emitter's SSE-response alloc-failure cleanup arms.
//   - test_sse_schema_oneof_fragment_overflow_omits_content and
//     test_sse_schema_oneof_fragment_long_name_truncation_omits_content --
//     sole cover for the tree emitter's own
//     "build_sse_oneof_fragment() returned false" glue branch
//     (bb_openapi_emit_tree.c's build_operation(), not the shared
//     build_sse_oneof_fragment() helper itself, which the ported
//     stream-path counterpart below already exercises independently).
//   - test_sse_schema_no_sse_topic_no_oneof -- sole cover for the tree
//     emitter's `if (sse_schema_count() > 0)` FALSE arm (bb_openapi_emit_
//     tree.c:139); this is branch-only (the line itself always executes),
//     so it doesn't show up as an uncovered LINE, only as a lost branch hit
//     -- exactly the shape that slipped past the local gate on PR #1069.
//     The stream-path arm has its own dedicated counterpart,
//     test_openapi_emit_stream_sse_zero_topics_omits_content in
//     test_openapi_emit.c, which cross-references this test by name.
// Every other test in this file ports 1:1 onto test_openapi_capture()
// (bb_openapi_emit_stream()).
#include "unity.h"
#include "bb_openapi.h"
#include "bb_openapi_emit_internal.h"
#include "bb_openapi_emit_tree_priv.h"
#include "bb_openapi_validate_priv.h"
#include "bb_http.h"
#include "bb_http_server.h"
#include "bb_http_host.h"
#include "bb_json.h"
#include "bb_json_test_hooks.h"
#include "test_openapi_capture.h"

#include <cJSON.h>
#include <stdio.h>
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

// Deliberately retained on the TREE path (bb_openapi_emit()), not ported --
// sole cover for bb_openapi_emit_tree.c:139's `if (sse_schema_count() > 0)`
// FALSE arm (no SSE-topic schemas registered, so build_operation() skips the
// oneOf-fragment block entirely). The stream emitter's equivalent arm already
// has its own dedicated counterpart --
// test_openapi_emit_stream_sse_zero_topics_omits_content in
// test_openapi_emit.c, which cross-references this test by name. Retained
// until B1-1054 PR 6 deletes the tree emitter.
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
// OOM: oneOf block — content/media alloc failures
// Line 424: components alloc failure
//
// Alloc sequence for s_sse_route (tag="events") + 1 LogEvent SSE schema:
//   root=0, info=1, paths_obj=2, path_item=3, op=4, tags=5, responses=6,
//   resp_obj=7, content=8, media=9, components=10, schemas=11.
// The oneOf fragment itself is a plain string (build_sse_oneof_fragment +
// bb_json_obj_set_raw) — no separate schema/one_of/ref bb_json_t allocations
// remain to fail independently.
// ---------------------------------------------------------------------------

// Deliberately retained on the TREE path (bb_openapi_emit()), not ported --
// sole cover for the tree emitter's SSE-response alloc-failure cleanup arms.
// Retained until B1-1054 PR 6 deletes the tree emitter.
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

// Deliberately retained on the TREE path (bb_openapi_emit()), not ported --
// sole cover for the tree emitter's SSE-response alloc-failure cleanup arms.
// Retained until B1-1054 PR 6 deletes the tree emitter.
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

// Deliberately retained on the TREE path (bb_openapi_emit()), not ported --
// sole cover for the tree emitter's SSE-response alloc-failure cleanup arms.
// Retained until B1-1054 PR 6 deletes the tree emitter.
// components=NULL → if(NULL && schemas) short-circuits (line 424 branch 1)
void test_sse_schema_oom_components_obj_skips_section(void)
{
    bb_http_route_registry_clear();
    bb_http_register_described_route(NULL, &s_sse_route);
    bb_openapi_register_schema("LogEvent", k_log_schema, "log");

    bb_openapi_meta_t meta = { .title = "T", .version = "1.0" };
    // content=8, media=9 succeed; components=10 fails
    bb_json_host_force_alloc_fail_after(10);
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

// 24 registry-cap entries, each with a component name at the widest length
// that still fits ref_val's own 80-byte budget untruncated (58 chars): the
// resulting fragment (~2195 bytes) exceeds BB_OPENAPI_SSE_ONEOF_BUF_SIZE
// (~1933 bytes) — the pathological case the buffer is NOT sized for. Content
// must be omitted, never emitted as truncated (malformed) JSON.
// Deliberately retained on the TREE path (bb_openapi_emit()), not ported --
// sole cover for the tree emitter's own fragment glue at
// bb_openapi_emit_tree.c:154-165 (distinct from the shared
// build_sse_oneof_fragment() helper, which the stream-path counterpart below
// already exercises independently). Retained until B1-1054 PR 6 deletes the
// tree emitter.
void test_sse_schema_oneof_fragment_overflow_omits_content(void)
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

    bb_openapi_meta_t meta = { .title = "T", .version = "1.0" };
    bb_json_t doc = bb_openapi_emit(&meta);
    TEST_ASSERT_NOT_NULL(doc);

    bb_json_t paths  = bb_json_obj_get_item(doc, "paths");
    bb_json_t pi     = bb_json_obj_get_item(paths, "/api/events");
    bb_json_t get_op = bb_json_obj_get_item(pi, "get");
    bb_json_t resps  = bb_json_obj_get_item(get_op, "responses");
    bb_json_t r200   = bb_json_obj_get_item(resps, "200");
    TEST_ASSERT_NOT_NULL(r200);
    // Fragment overflowed the bounded buffer — content omitted, not truncated.
    TEST_ASSERT_NULL(bb_json_obj_get_item(r200, "content"));

    bb_json_free(doc);
}

// A component name long enough that "#/components/schemas/<name>" truncates
// inside ref_val's own 80-byte budget (name > 58 chars) must not silently
// splice a truncated (wrong-but-valid-looking) $ref into the fragment —
// that would be worse than the overflow case above, since the output reads
// as valid JSON while pointing at the wrong schema. Content must be omitted.
// Deliberately retained on the TREE path (bb_openapi_emit()), not ported --
// sole cover for the tree emitter's own fragment glue at
// bb_openapi_emit_tree.c:154-165 (distinct from the shared
// build_sse_oneof_fragment() helper, which the stream-path counterpart below
// already exercises independently). Retained until B1-1054 PR 6 deletes the
// tree emitter.
void test_sse_schema_oneof_fragment_long_name_truncation_omits_content(void)
{
    bb_http_route_registry_clear();
    bb_http_register_described_route(NULL, &s_sse_route);

    static char long_name[70];
    memset(long_name, 'A', sizeof(long_name) - 1);
    long_name[sizeof(long_name) - 1] = '\0';

    TEST_ASSERT_EQUAL(BB_OK,
        bb_openapi_register_schema(long_name, k_log_schema, "log"));

    bb_openapi_meta_t meta = { .title = "T", .version = "1.0" };
    bb_json_t doc = bb_openapi_emit(&meta);
    TEST_ASSERT_NOT_NULL(doc);

    bb_json_t paths  = bb_json_obj_get_item(doc, "paths");
    bb_json_t pi     = bb_json_obj_get_item(paths, "/api/events");
    bb_json_t get_op = bb_json_obj_get_item(pi, "get");
    bb_json_t resps  = bb_json_obj_get_item(get_op, "responses");
    bb_json_t r200   = bb_json_obj_get_item(resps, "200");
    TEST_ASSERT_NOT_NULL(r200);
    TEST_ASSERT_NULL(bb_json_obj_get_item(r200, "content"));

    bb_json_free(doc);
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

// Stream-path counterpart to test_sse_schema_oneof_fragment_overflow_omits_content
// — same oversized registry, but through bb_openapi_emit_stream()'s
// emit_operation() (B1-1116 PR4), pinning that the overflow-omit branch
// behaves identically on both emitters.
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
