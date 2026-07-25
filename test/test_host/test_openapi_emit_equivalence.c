#include "unity.h"
#include "bb_openapi.h"
#include "bb_openapi_emit_tree_priv.h"
#include "bb_http.h"
#include "bb_http_server.h"
#include "bb_http_host.h"
#include "bb_json.h"

#include <cJSON.h>
#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------------
// B1-1116 PR1 — stream-vs-tree structural equivalence harness.
//
// bb_openapi_emit() (tree) and bb_openapi_emit_stream() (chunked) must
// describe the SAME OpenAPI document. This suite is a lock: it lands FIRST
// so a later rewrite of either emitter (PR2+) trips a failure here if the
// two paths drift.
//
// STRUCTURAL equivalence, not byte equality: the tree path splices schema
// literals via bb_json_obj_set_raw, which parses and re-serializes them
// (cJSON may renormalize whitespace/key order/number formatting); the stream
// path splices the same literals as raw bytes. Both documents are therefore
// parsed with cJSON and compared with cJSON_Compare, which already treats
// object key order as irrelevant and array order as significant — exactly
// the invariant this suite needs.
//
// meta->title == NULL and meta->version == NULL are both covered below
// (fixtures 12/13): both emitters now default a NULL title to
// "breadboard device" and a NULL version to "0.0.0", reconciled in the
// immediate B1-1116 follow-up that fixed the tree emitter's prior ""
// title default (components/bb_openapi/src/bb_openapi_emit.c).
// ---------------------------------------------------------------------------

static bb_err_t stub_handler(bb_http_request_t *req) { (void)req; return BB_OK; }

// Build the tree document and return it as a malloc'd cJSON string
// (caller must free with bb_json_free_str).
static char *tree_doc_str(const bb_openapi_meta_t *meta)
{
    bb_json_t doc = bb_openapi_emit(meta);
    TEST_ASSERT_NOT_NULL(doc);
    char *s = bb_json_serialize(doc);
    bb_json_free(doc);
    TEST_ASSERT_NOT_NULL(s);
    return s;
}

// Run the streaming emitter and capture its body (caller must call
// bb_http_host_capture_free on the returned capture).
static bb_http_host_capture_t stream_doc_capture(const bb_openapi_meta_t *meta)
{
    bb_http_request_t *req = NULL;
    bb_http_host_capture_begin(&req);

    TEST_ASSERT_EQUAL(BB_OK, bb_openapi_emit_stream(req, meta));

    bb_http_host_capture_t cap = {0};
    bb_http_host_capture_end(req, &cap);
    TEST_ASSERT_NOT_NULL(cap.body);
    return cap;
}

// Assert the tree and stream emitters produce structurally equivalent
// documents for the routes/schemas currently registered.
static void assert_stream_equivalent_to_tree(const bb_openapi_meta_t *meta)
{
    char *tree_str = tree_doc_str(meta);
    bb_http_host_capture_t cap = stream_doc_capture(meta);

    cJSON *tree_json   = cJSON_Parse(tree_str);
    cJSON *stream_json = cJSON_Parse(cap.body);

    TEST_ASSERT_NOT_NULL_MESSAGE(tree_json, "tree doc failed to parse as JSON");
    TEST_ASSERT_NOT_NULL_MESSAGE(stream_json, "stream doc failed to parse as JSON");

    if (tree_json && stream_json) {
        bool equal = cJSON_Compare(tree_json, stream_json, true);
        if (!equal) {
            char msg[512];
            snprintf(msg, sizeof(msg),
                     "tree/stream docs diverge\n--- tree ---\n%s\n--- stream ---\n%s",
                     tree_str, cap.body);
            TEST_FAIL_MESSAGE(msg);
        }
    }

    if (tree_json)   cJSON_Delete(tree_json);
    if (stream_json) cJSON_Delete(stream_json);
    bb_json_free_str(tree_str);
    bb_http_host_capture_free(&cap);
}

// ---------------------------------------------------------------------------
// Fixture 1: no routes registered
// ---------------------------------------------------------------------------

void test_openapi_emit_equivalence_no_routes(void)
{
    bb_http_route_registry_clear();
    bb_openapi_schema_registry_clear();

    bb_openapi_meta_t meta = { .title = "Test", .version = "1.0.0" };
    assert_stream_equivalent_to_tree(&meta);
}

// ---------------------------------------------------------------------------
// Fixture 2: single GET route
// ---------------------------------------------------------------------------

static const bb_route_response_t s_get_responses[] = {
    { .status = 200, .content_type = "application/json",
      .schema = "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"integer\"}}}",
      .description = "foo response" },
    { .status = 0 },
};

static const bb_route_t s_route_get = {
    .method = BB_HTTP_GET, .path = "/api/foo", .tag = "foo-tag",
    .summary = "Get foo resource", .operation_id = "getFoo",
    .responses = s_get_responses, .handler = stub_handler,
};

void test_openapi_emit_equivalence_single_get_route(void)
{
    bb_http_route_registry_clear();
    bb_openapi_schema_registry_clear();
    bb_http_register_described_route(NULL, &s_route_get);

    bb_openapi_meta_t meta = { .title = "Test", .version = "1.0.0" };
    assert_stream_equivalent_to_tree(&meta);
}

// ---------------------------------------------------------------------------
// Fixture 3: route with parameters
// ---------------------------------------------------------------------------

static const bb_route_param_t s_params[] = {
    { .name = "topic", .in = "query", .description = "topic filter",
      .required = false, .schema_type = "string" },
    { .name = "id", .in = "path", .description = "resource id",
      .required = true, .schema_type = "integer" },
};

static const bb_route_t s_route_with_params = {
    .method = BB_HTTP_GET, .path = "/api/items/{id}", .tag = "items",
    .summary = "Get item", .operation_id = "getItem",
    .responses = s_get_responses, .handler = stub_handler,
    .parameters = s_params, .parameters_count = 2,
};

void test_openapi_emit_equivalence_route_with_parameters(void)
{
    bb_http_route_registry_clear();
    bb_openapi_schema_registry_clear();
    bb_http_register_described_route(NULL, &s_route_with_params);

    bb_openapi_meta_t meta = { .title = "Test", .version = "1.0.0" };
    assert_stream_equivalent_to_tree(&meta);
}

// ---------------------------------------------------------------------------
// Fixture 4: route with request_schema (requestBody)
// ---------------------------------------------------------------------------

static const bb_route_response_t s_post_responses[] = {
    { .status = 201, .content_type = "application/json",
      .schema = "{\"type\":\"object\",\"properties\":{\"created\":{\"type\":\"boolean\"}}}",
      .description = "bar created" },
    { .status = 0 },
};

static const bb_route_t s_route_post_with_body = {
    .method = BB_HTTP_POST, .path = "/api/bar", .tag = "bar-tag",
    .summary = "Create bar resource", .operation_id = "createBar",
    .request_content_type = "application/json",
    .request_schema = "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"}}}",
    .responses = s_post_responses, .handler = stub_handler,
};

void test_openapi_emit_equivalence_route_with_request_schema(void)
{
    bb_http_route_registry_clear();
    bb_openapi_schema_registry_clear();
    bb_http_register_described_route(NULL, &s_route_post_with_body);

    bb_openapi_meta_t meta = { .title = "Test", .version = "1.0.0" };
    assert_stream_equivalent_to_tree(&meta);
}

// ---------------------------------------------------------------------------
// Fixture 5: multi-method on one path
// ---------------------------------------------------------------------------

static const bb_route_t s_route_multi_get = {
    .method = BB_HTTP_GET, .path = "/api/multi", .tag = "multi",
    .summary = "Multi GET", .operation_id = "getMulti",
    .responses = s_get_responses, .handler = stub_handler,
};

static const bb_route_t s_route_multi_post = {
    .method = BB_HTTP_POST, .path = "/api/multi", .tag = "multi",
    .summary = "Multi POST", .operation_id = "postMulti",
    .request_content_type = "application/json",
    .request_schema = "{\"type\":\"object\"}",
    .responses = s_post_responses, .handler = stub_handler,
};

void test_openapi_emit_equivalence_multi_method_same_path(void)
{
    bb_http_route_registry_clear();
    bb_openapi_schema_registry_clear();
    bb_http_register_described_route(NULL, &s_route_multi_get);
    bb_http_register_described_route(NULL, &s_route_multi_post);

    bb_openapi_meta_t meta = { .title = "Test", .version = "1.0.0" };
    assert_stream_equivalent_to_tree(&meta);
}

// ---------------------------------------------------------------------------
// Fixture 6: multi-path
// ---------------------------------------------------------------------------

static const bb_route_t s_route_path_a = {
    .method = BB_HTTP_GET, .path = "/api/alpha", .tag = "alpha",
    .summary = "Alpha", .operation_id = "getAlpha",
    .responses = s_get_responses, .handler = stub_handler,
};

static const bb_route_t s_route_path_b = {
    .method = BB_HTTP_DELETE, .path = "/api/beta", .tag = "beta",
    .summary = "Beta", .operation_id = "deleteBeta",
    .responses = s_get_responses, .handler = stub_handler,
};

void test_openapi_emit_equivalence_multi_path(void)
{
    bb_http_route_registry_clear();
    bb_openapi_schema_registry_clear();
    bb_http_register_described_route(NULL, &s_route_path_a);
    bb_http_register_described_route(NULL, &s_route_path_b);

    bb_openapi_meta_t meta = { .title = "Test", .version = "1.0.0" };
    assert_stream_equivalent_to_tree(&meta);
}

// ---------------------------------------------------------------------------
// Fixture 7: SSE route exercising oneOf synthesis, schema registry populated
// ---------------------------------------------------------------------------

static const char k_log_schema[] =
    "{\"title\":\"LogEvent\",\"x-sse-topic\":\"log\",\"type\":\"object\","
    "\"properties\":{\"ts\":{\"type\":\"integer\"},\"msg\":{\"type\":\"string\"}},"
    "\"required\":[\"ts\",\"msg\"]}";

static const char k_wifi_schema[] =
    "{\"title\":\"WifiInfo\",\"type\":\"object\","
    "\"properties\":{\"ssid\":{\"type\":\"string\"}},\"required\":[\"ssid\"]}";

static const bb_route_response_t s_sse_responses[] = {
    { .status = 200, .content_type = "text/event-stream",
      .schema = NULL, .description = "SSE stream" },
    { .status = 0 },
};

static const bb_route_t s_route_sse = {
    .method = BB_HTTP_GET, .path = "/api/events", .tag = "events",
    .summary = "SSE stream", .responses = s_sse_responses, .handler = stub_handler,
};

void test_openapi_emit_equivalence_sse_oneof_synthesis(void)
{
    bb_http_route_registry_clear();
    bb_openapi_schema_registry_clear();
    bb_http_register_described_route(NULL, &s_route_sse);
    bb_openapi_register_schema("LogEvent", k_log_schema, "log");
    // REST-only schema (no sse_topic) must NOT appear in oneOf on either path.
    bb_openapi_register_schema("WifiInfo", k_wifi_schema, NULL);

    bb_openapi_meta_t meta = { .title = "Test", .version = "1.0.0" };

    // Assert both documents agree on the whole tree...
    assert_stream_equivalent_to_tree(&meta);

    // ...and specifically on the oneOf branch (the branch PR2 rewrites).
    char *tree_str = tree_doc_str(&meta);
    bb_http_host_capture_t cap = stream_doc_capture(&meta);

    TEST_ASSERT_NOT_NULL(strstr(tree_str, "\"oneOf\""));
    TEST_ASSERT_NOT_NULL(strstr(cap.body, "\"oneOf\""));
    TEST_ASSERT_NOT_NULL(strstr(tree_str, "\"$ref\":\"#/components/schemas/LogEvent\""));
    TEST_ASSERT_NOT_NULL(strstr(cap.body, "\"$ref\":\"#/components/schemas/LogEvent\""));
    TEST_ASSERT_NULL(strstr(tree_str, "\"$ref\":\"#/components/schemas/WifiInfo\""));
    TEST_ASSERT_NULL(strstr(cap.body, "\"$ref\":\"#/components/schemas/WifiInfo\""));

    bb_json_free_str(tree_str);
    bb_http_host_capture_free(&cap);
}

// ---------------------------------------------------------------------------
// Fixture 8: components/schemas populated vs empty
// ---------------------------------------------------------------------------

void test_openapi_emit_equivalence_components_schemas_populated(void)
{
    bb_http_route_registry_clear();
    bb_openapi_schema_registry_clear();
    bb_http_register_described_route(NULL, &s_route_get);
    bb_openapi_register_schema("WifiInfo", k_wifi_schema, NULL);

    bb_openapi_meta_t meta = { .title = "Test", .version = "1.0.0" };
    assert_stream_equivalent_to_tree(&meta);
}

void test_openapi_emit_equivalence_components_schemas_empty(void)
{
    bb_http_route_registry_clear();
    bb_openapi_schema_registry_clear();
    bb_http_register_described_route(NULL, &s_route_get);
    // registry left empty — no components/schemas section on either path

    bb_openapi_meta_t meta = { .title = "Test", .version = "1.0.0" };
    assert_stream_equivalent_to_tree(&meta);
}

// ---------------------------------------------------------------------------
// Fixture 9: servers + description populated — exercises the small info/
// servers subtrees both emitters build independently.
// ---------------------------------------------------------------------------

void test_openapi_emit_equivalence_servers_and_description(void)
{
    bb_http_route_registry_clear();
    bb_openapi_schema_registry_clear();
    bb_http_register_described_route(NULL, &s_route_get);

    bb_openapi_meta_t meta = {
        .title = "Test", .version = "1.0.0",
        .description = "A test API", .server_url = "http://miner.local",
    };
    assert_stream_equivalent_to_tree(&meta);
}

// ---------------------------------------------------------------------------
// Fixture 10: route->responses == NULL — build_operation() guards this
// (components/bb_openapi/src/bb_openapi_emit.c:269:
//  `if (responses && route->responses)`), and today it's safe only because
// both emitters call the SAME build_operation() for the non-streamed parts
// of an operation. That shared-code safety net goes away once the emitters
// fork (PR 4), so pin the shape now.
// ---------------------------------------------------------------------------

static const bb_route_t s_route_null_responses = {
    .method = BB_HTTP_GET, .path = "/api/null-responses", .tag = "test",
    .summary = "No responses array", .operation_id = "getNullResponses",
    .responses = NULL, .handler = stub_handler,
};

void test_openapi_emit_equivalence_null_responses(void)
{
    bb_http_route_registry_clear();
    bb_openapi_schema_registry_clear();
    bb_http_register_described_route(NULL, &s_route_null_responses);

    bb_openapi_meta_t meta = { .title = "Test", .version = "1.0.0" };
    assert_stream_equivalent_to_tree(&meta);
}

// ---------------------------------------------------------------------------
// Fixture 11: deeply nested response schema — NOT a depth-cap tripwire.
//
// bb_http_json_obj caps nesting at BB_JSON_OBJ_MAX_DEPTH == 8
// (components/bb_http_server/src/bb_http_json_obj.c:14), and PR 4 moves the
// stream emitter's operation building onto that incremental builder. But
// this fixture cannot guard that cap: BOTH emitters splice the schema
// literal as opaque bytes (bb_json_obj_set_raw on the tree side, a
// byte-for-byte chunk write on the stream side), so nesting the schema
// CONTENT adds zero exposure — a 1-level schema and this 10-level one are
// equally invisible to any depth counter. The real depth pressure is in the
// ENVELOPE both emitters already build around the schema (paths ->
// path-item -> operation -> responses -> status -> content -> media == 7
// levels before "schema" is even reached), which every fixture in this file
// already exercises at the same depth as this one. No fixture in this suite
// has a construction path that could trip BB_JSON_OBJ_MAX_DEPTH; that
// requires a dedicated test in PR 4 once the depth-capped construction path
// actually exists. This fixture is kept because it's harmless and does pin
// envelope/schema round-tripping through a schema with real internal
// structure — just don't credit it with depth-cap coverage it can't provide.
// ---------------------------------------------------------------------------

static const char k_deep_schema[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"a\":{\"type\":\"object\",\"properties\":{"
    "\"b\":{\"type\":\"array\",\"items\":{\"type\":\"object\",\"properties\":{"
    "\"c\":{\"type\":\"object\",\"properties\":{"
    "\"d\":{\"type\":\"string\"}"
    "}}"
    "}}}"
    "}}"
    "}}";

static const bb_route_response_t s_deep_responses[] = {
    { .status = 200, .content_type = "application/json",
      .schema = k_deep_schema, .description = "deeply nested" },
    { .status = 0 },
};

static const bb_route_t s_route_deep_schema = {
    .method = BB_HTTP_GET, .path = "/api/deep", .tag = "deep",
    .summary = "Deeply nested response schema", .operation_id = "getDeep",
    .responses = s_deep_responses, .handler = stub_handler,
};

void test_openapi_emit_equivalence_deeply_nested_schema(void)
{
    bb_http_route_registry_clear();
    bb_openapi_schema_registry_clear();
    bb_http_register_described_route(NULL, &s_route_deep_schema);

    bb_openapi_meta_t meta = { .title = "Test", .version = "1.0.0" };
    assert_stream_equivalent_to_tree(&meta);
}

// ---------------------------------------------------------------------------
// Fixture 12: meta->title == NULL — both emitters must default to
// "breadboard device" (components/bb_openapi/src/bb_openapi_emit.c).
// ---------------------------------------------------------------------------

void test_openapi_emit_equivalence_null_title_defaults_agree(void)
{
    bb_http_route_registry_clear();
    bb_openapi_schema_registry_clear();
    bb_http_register_described_route(NULL, &s_route_get);

    bb_openapi_meta_t meta = { .title = NULL, .version = "1.0.0" };
    assert_stream_equivalent_to_tree(&meta);

    char *tree_str = tree_doc_str(&meta);
    bb_http_host_capture_t cap = stream_doc_capture(&meta);

    TEST_ASSERT_NOT_NULL(strstr(tree_str, "\"title\":\"breadboard device\""));
    TEST_ASSERT_NOT_NULL(strstr(cap.body, "\"title\":\"breadboard device\""));

    bb_json_free_str(tree_str);
    bb_http_host_capture_free(&cap);
}

// ---------------------------------------------------------------------------
// Fixture 13: meta->version == NULL — pins the symmetry that already holds
// (both emitters default a NULL version to "0.0.0").
// ---------------------------------------------------------------------------

void test_openapi_emit_equivalence_null_version_defaults_agree(void)
{
    bb_http_route_registry_clear();
    bb_openapi_schema_registry_clear();
    bb_http_register_described_route(NULL, &s_route_get);

    bb_openapi_meta_t meta = { .title = "Test", .version = NULL };
    assert_stream_equivalent_to_tree(&meta);

    char *tree_str = tree_doc_str(&meta);
    bb_http_host_capture_t cap = stream_doc_capture(&meta);

    TEST_ASSERT_NOT_NULL(strstr(tree_str, "\"version\":\"0.0.0\""));
    TEST_ASSERT_NOT_NULL(strstr(cap.body, "\"version\":\"0.0.0\""));

    bb_json_free_str(tree_str);
    bb_http_host_capture_free(&cap);
}
