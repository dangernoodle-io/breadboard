// Schema-literal sanity for routes registered via bb_http_register_described_route
// or bb_http_register_route_descriptor_only.
//
// Walks bb_http_route_registry_foreach (the same iterator the OpenAPI emitter
// uses) and asserts that every responses[].schema and request_schema string
// literal parses as JSON. Catches the bb_manifest-class regression where a
// hand-authored schema literal is malformed and bb_json_obj_set_raw silently
// substitutes JSON null in the spec.
//
// The host build today does not auto-link every component's init function,
// so this test seeds the registry with explicit fixtures that mirror the
// real-world schemas worth guarding. Add a fixture here whenever a new
// hand-authored schema literal appears in production code.

#include "unity.h"
#include "bb_http.h"
#include "bb_json.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"

// ---------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------

// Mirrors platform/espidf/bb_manifest/bb_manifest_register.c. Copy-pasted
// intentionally: any future edit to the production literal must also update
// this string, which forces a re-validation pass through the test.
static const char k_manifest_schema[] =
    "{\"type\":\"object\","
    "\"properties\":{"
    "\"nvs\":{\"type\":\"array\","
    "\"items\":{\"type\":\"object\","
    "\"properties\":{"
    "\"namespace\":{\"type\":\"string\"},"
    "\"keys\":{\"type\":\"array\","
    "\"items\":{\"type\":\"object\","
    "\"properties\":{"
    "\"key\":{\"type\":\"string\"},"
    "\"type\":{\"type\":\"string\"},"
    "\"default\":{\"type\":\"string\"},"
    "\"desc\":{\"type\":\"string\"},"
    "\"reboot_required\":{\"type\":\"boolean\"},"
    "\"provisioning_only\":{\"type\":\"boolean\"}}"
    "}}}},"
    "\"mdns\":{\"type\":\"array\","
    "\"items\":{\"type\":\"object\","
    "\"properties\":{"
    "\"service\":{\"type\":\"string\"},"
    "\"txt\":{\"type\":\"array\","
    "\"items\":{\"type\":\"object\","
    "\"properties\":{"
    "\"key\":{\"type\":\"string\"},"
    "\"desc\":{\"type\":\"string\"},"
    "\"values\":{\"type\":\"string\"}}}}}}}}}}";

static const bb_route_response_t s_manifest_responses[] = {
    { 200, "application/json", k_manifest_schema, "manifest schema fixture" },
    { 0 },
};

static const bb_route_t s_manifest_route = {
    .method    = BB_HTTP_GET,
    .path      = "/api/manifest",
    .tag       = "manifest",
    .summary   = "manifest schema fixture",
    .responses = s_manifest_responses,
    .handler   = NULL,
};

// ---------------------------------------------------------------------------
// Walker
// ---------------------------------------------------------------------------

typedef struct {
    int  total_schemas;
    int  failures;
    char first_failure[256];
} schema_check_ctx_t;

static void check_schema_string(const char *path, const char *which,
                                const char *schema, schema_check_ctx_t *ctx)
{
    if (!schema) return;
    ctx->total_schemas++;
    cJSON *parsed = cJSON_Parse(schema);
    if (!parsed) {
        if (ctx->failures == 0) {
            const char *err = cJSON_GetErrorPtr();
            long off = err ? (long)(err - schema) : -1;
            snprintf(ctx->first_failure, sizeof(ctx->first_failure),
                     "%s [%s]: malformed JSON at offset %ld", path, which, off);
        }
        ctx->failures++;
        return;
    }
    cJSON_Delete(parsed);
}

static void route_schema_walker(const bb_route_t *route, void *ctx_)
{
    schema_check_ctx_t *ctx = (schema_check_ctx_t *)ctx_;
    if (!route) return;
    check_schema_string(route->path, "request", route->request_schema, ctx);
    if (route->responses) {
        for (const bb_route_response_t *r = route->responses; r->status != 0; r++) {
            check_schema_string(route->path, "response", r->schema, ctx);
        }
    }
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

void test_route_schemas_manifest_fixture_parses(void)
{
    cJSON *parsed = cJSON_Parse(k_manifest_schema);
    TEST_ASSERT_NOT_NULL_MESSAGE(parsed,
        "bb_manifest schema literal must parse as valid JSON");
    cJSON_Delete(parsed);
}

void test_route_schemas_registry_all_valid(void)
{
    bb_http_route_registry_clear();
    bb_err_t err = bb_http_register_route_descriptor_only(&s_manifest_route);
    TEST_ASSERT_EQUAL(BB_OK, err);

    schema_check_ctx_t ctx = { 0 };
    bb_http_route_registry_foreach(route_schema_walker, &ctx);

    TEST_ASSERT_GREATER_THAN_INT(0, ctx.total_schemas);
    if (ctx.failures > 0) {
        TEST_FAIL_MESSAGE(ctx.first_failure);
    }

    bb_http_route_registry_clear();
}

void test_route_schemas_walker_flags_malformed(void)
{
    static const bb_route_response_t bad_responses[] = {
        // 1 unbalanced brace — must trip the walker.
        { 200, "application/json", "{\"type\":\"object\"", "deliberately bad" },
        { 0 },
    };
    static const bb_route_t bad_route = {
        .method    = BB_HTTP_GET,
        .path      = "/api/bad",
        .responses = bad_responses,
    };

    bb_http_route_registry_clear();
    TEST_ASSERT_EQUAL(BB_OK, bb_http_register_route_descriptor_only(&bad_route));

    schema_check_ctx_t ctx = { 0 };
    bb_http_route_registry_foreach(route_schema_walker, &ctx);

    TEST_ASSERT_EQUAL_INT(1, ctx.total_schemas);
    TEST_ASSERT_EQUAL_INT(1, ctx.failures);

    bb_http_route_registry_clear();
}

void test_set_raw_writes_null_on_malformed_json(void)
{
    bb_json_t obj = bb_json_obj_new();
    TEST_ASSERT_NOT_NULL(obj);

    bb_json_obj_set_raw(obj, "schema", "{\"oops\":");

    char *out = bb_json_serialize(obj);
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(out, "\"schema\":null"),
        "set_raw should write JSON null on cJSON_Parse failure");
    bb_json_free_str(out);
    bb_json_free(obj);
}

void test_set_raw_writes_parsed_object_on_valid_json(void)
{
    bb_json_t obj = bb_json_obj_new();
    TEST_ASSERT_NOT_NULL(obj);

    bb_json_obj_set_raw(obj, "schema",
                        "{\"type\":\"object\",\"properties\":{\"x\":{\"type\":\"integer\"}}}");

    char *out = bb_json_serialize(obj);
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_NOT_NULL(strstr(out, "\"type\":\"object\""));
    TEST_ASSERT_NULL_MESSAGE(strstr(out, "null"),
        "valid schema should not be substituted with null");
    bb_json_free_str(out);
    bb_json_free(obj);
}
