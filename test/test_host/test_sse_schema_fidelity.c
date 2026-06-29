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
        "S0","S1","S2","S3","S4","S5","S6","S7"
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
