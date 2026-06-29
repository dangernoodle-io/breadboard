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

// Mirrors platform/espidf/bb_wifi/bb_wifi_routes.c (s_wifi_patch_route.request_schema).
// Copy-pasted intentionally: edits to the production literal must update this too.
// Also guards that the schema declares requestBody + requires ssid (not just parses).
static const char k_wifi_patch_request_schema[] =
    "{\"type\":\"object\","
    "\"properties\":{\"ssid\":{\"type\":\"string\",\"maxLength\":31},"
    "\"password\":{\"type\":\"string\",\"maxLength\":63}},"
    "\"required\":[\"ssid\"]}";

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

static const bb_route_response_t s_wifi_patch_responses[] = {
    { 202, "application/json",
      "{\"type\":\"object\","
      "\"properties\":{\"status\":{\"type\":\"string\"}},"
      "\"required\":[\"status\"]}",
      "reconfigure accepted" },
    { 400, "application/json",
      "{\"type\":\"object\","
      "\"properties\":{\"error\":{\"type\":\"string\"}},"
      "\"required\":[\"error\"]}",
      "bad request" },
    { 0 },
};

static const bb_route_t s_wifi_patch_route = {
    .method               = BB_HTTP_PATCH,
    .path                 = "/api/wifi",
    .tag                  = "wifi",
    .summary              = "Stage new Wi-Fi credentials and arm deferred reboot",
    .request_content_type = "application/json",
    .request_schema       = k_wifi_patch_request_schema,
    .responses            = s_wifi_patch_responses,
    .handler              = NULL,
};

// Mirrors platform/espidf/bb_telemetry/bb_telemetry_routes.c (s_telemetry_patch_route.request_schema).
// Copy-pasted intentionally: edits to the production literal must update this too.
static const char k_telemetry_patch_request_schema[] =
    "{\"type\":\"object\","
    "\"description\":\"Keys are section names (mqtt, http, publisher); "
                     "values are section-specific patch objects\","
    "\"properties\":{"
    "\"mqtt\":{\"type\":\"object\","
        "\"properties\":{"
        "\"uri\":{\"type\":\"string\"},"
        "\"client_id\":{\"type\":\"string\"},"
        "\"username\":{\"type\":\"string\"},"
        "\"password\":{\"type\":\"string\"},"
        "\"tls_ca\":{\"type\":\"string\"},"
        "\"tls_cert\":{\"type\":\"string\"},"
        "\"tls_key\":{\"type\":\"string\"},"
        "\"tls\":{\"type\":\"boolean\"},"
        "\"enabled\":{\"type\":\"boolean\"}}},"
    "\"http\":{\"type\":\"object\","
        "\"properties\":{"
        "\"base\":{\"type\":\"string\"},"
        "\"path_tmpl\":{\"type\":\"string\"},"
        "\"client_id\":{\"type\":\"string\"},"
        "\"qos\":{\"type\":\"integer\"},"
        "\"tls_ca\":{\"type\":\"string\"},"
        "\"tls_cert\":{\"type\":\"string\"},"
        "\"tls_key\":{\"type\":\"string\"},"
        "\"headers\":{\"type\":\"array\",\"items\":{\"type\":\"object\"}},"
        "\"enabled\":{\"type\":\"boolean\"}}},"
    "\"publisher\":{\"type\":\"object\","
        "\"properties\":{"
        "\"interval_ms\":{\"type\":\"integer\",\"minimum\":1000,\"maximum\":3600000},"
        "\"enabled\":{\"type\":\"boolean\"}}}}}";

// Mirrors components/bb_sensors/bb_sensors_schema_priv.h (k_sensors_patch_request_schema),
// non-autofan variant. Copy-pasted intentionally.
static const char k_sensors_patch_request_schema[] =
    "{\"type\":\"object\","
    "\"properties\":{"
    "\"fan\":{\"type\":\"object\","
        "\"properties\":{"
        "\"duty_pct\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":100}},"
        "\"required\":[\"duty_pct\"]}}}";

// PATCH /api/telemetry fixture route — used by registry_all_valid and desc_audit tests.
static const bb_route_response_t s_telemetry_patch_responses_fixture[] = {
    { 200, "application/json",
      "{\"type\":\"object\",\"properties\":{\"reboot_required\":{\"type\":\"boolean\"}},\"required\":[\"reboot_required\"]}",
      "patch applied" },
    { 0 },
};
static const bb_route_t s_telemetry_patch_route_fixture = {
    .method               = BB_HTTP_PATCH,
    .path                 = "/api/telemetry",
    .tag                  = "telemetry",
    .request_content_type = "application/json",
    .request_schema       = k_telemetry_patch_request_schema,
    .responses            = s_telemetry_patch_responses_fixture,
    .handler              = NULL,
};

// PATCH /api/sensors fixture route — used by registry_all_valid and desc_audit tests.
static const bb_route_response_t s_sensors_patch_responses_fixture[] = {
    { 204, NULL, NULL, "patch applied" },
    { 0 },
};
static const bb_route_t s_sensors_patch_route_fixture = {
    .method               = BB_HTTP_PATCH,
    .path                 = "/api/sensors",
    .tag                  = "sensors",
    .request_content_type = "application/json",
    .request_schema       = k_sensors_patch_request_schema,
    .responses            = s_sensors_patch_responses_fixture,
    .handler              = NULL,
};

// Mirrors platform/espidf/bb_telemetry/bb_telemetry_routes.c
// (s_telemetry_patch_responses[0].schema). Copy-pasted intentionally so that
// future edits to the production literal must also update this fixture.
// Validates that the B1-398 publisher_unavailable addition is valid JSON.
static const char k_telemetry_patch_200_schema[] =
    "{\"type\":\"object\","
    "\"properties\":{"
        "\"reboot_required\":{\"type\":\"boolean\"},"
        "\"publisher_unavailable\":{\"type\":\"boolean\","
            "\"description\":\"true when publisher.enabled=true was requested "
            "but the publisher worker is not compiled in on this build "
            "(AUTOREGISTER=n) — reboot will not start the publisher\"}"
    "},"
    "\"required\":[\"reboot_required\"]}";

// ---------------------------------------------------------------------------
// Walker
// ---------------------------------------------------------------------------

typedef struct {
    int  total_schemas;
    int  failures;
    int  mutating_bare_body;      // POST/PATCH/PUT with request_schema but no properties
    char first_failure[256];
    char first_bare_body[128];
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

// Returns true iff the JSON-Schema string has a non-empty "properties" object.
static bool check_request_schema_has_properties(const char *schema)
{
    if (!schema) return false;
    cJSON *parsed = cJSON_Parse(schema);
    if (!parsed) return false;
    cJSON *props = cJSON_GetObjectItemCaseSensitive(parsed, "properties");
    bool ok = props && cJSON_IsObject(props) && cJSON_GetArraySize(props) > 0;
    cJSON_Delete(parsed);
    return ok;
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
    // Guard: mutating routes with a request_schema must declare properties.
    bool is_mutating = (route->method == BB_HTTP_POST ||
                        route->method == BB_HTTP_PATCH ||
                        route->method == BB_HTTP_PUT);
    if (is_mutating && route->request_schema) {
        if (!check_request_schema_has_properties(route->request_schema)) {
            if (ctx->mutating_bare_body == 0) {
                snprintf(ctx->first_bare_body, sizeof(ctx->first_bare_body),
                         "%s (method %d): request_schema missing non-empty properties",
                         route->path ? route->path : "(null)", (int)route->method);
            }
            ctx->mutating_bare_body++;
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

// B1-398: guard the telemetry PATCH 200 response schema (new publisher_unavailable field).
void test_route_schemas_telemetry_patch_200_parses(void)
{
    cJSON *parsed = cJSON_Parse(k_telemetry_patch_200_schema);
    TEST_ASSERT_NOT_NULL_MESSAGE(parsed,
        "telemetry PATCH 200 schema (with publisher_unavailable) must parse as valid JSON");
    // Verify reboot_required is in required[].
    cJSON *props    = cJSON_GetObjectItemCaseSensitive(parsed, "properties");
    cJSON *req_arr  = cJSON_GetObjectItemCaseSensitive(parsed, "required");
    TEST_ASSERT_NOT_NULL_MESSAGE(props,   "schema must have 'properties'");
    TEST_ASSERT_NOT_NULL_MESSAGE(req_arr, "schema must have 'required'");
    TEST_ASSERT_TRUE_MESSAGE(cJSON_IsArray(req_arr), "'required' must be array");
    bool found_rr = false;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, req_arr) {
        if (cJSON_IsString(item) && strcmp(item->valuestring, "reboot_required") == 0)
            found_rr = true;
    }
    TEST_ASSERT_TRUE_MESSAGE(found_rr, "reboot_required must be in required[]");
    // publisher_unavailable must be a property (optional, not in required[]).
    cJSON *pu = cJSON_GetObjectItemCaseSensitive(props, "publisher_unavailable");
    TEST_ASSERT_NOT_NULL_MESSAGE(pu, "publisher_unavailable property must exist");
    cJSON_Delete(parsed);
}

void test_route_schemas_registry_all_valid(void)
{
    bb_http_route_registry_clear();
    bb_err_t err = bb_http_register_route_descriptor_only(&s_manifest_route);
    TEST_ASSERT_EQUAL(BB_OK, err);
    err = bb_http_register_route_descriptor_only(&s_wifi_patch_route);
    TEST_ASSERT_EQUAL(BB_OK, err);
    err = bb_http_register_route_descriptor_only(&s_telemetry_patch_route_fixture);
    TEST_ASSERT_EQUAL(BB_OK, err);
    err = bb_http_register_route_descriptor_only(&s_sensors_patch_route_fixture);
    TEST_ASSERT_EQUAL(BB_OK, err);

    schema_check_ctx_t ctx = { 0 };
    bb_http_route_registry_foreach(route_schema_walker, &ctx);

    TEST_ASSERT_GREATER_THAN_INT(0, ctx.total_schemas);
    if (ctx.failures > 0) {
        TEST_FAIL_MESSAGE(ctx.first_failure);
    }
    if (ctx.mutating_bare_body > 0) {
        TEST_FAIL_MESSAGE(ctx.first_bare_body);
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

// Guard: PATCH /api/wifi request_schema declares a requestBody and requires ssid.
// If the production literal omits request_schema or drops the ssid requirement,
// this test trips before the spec regression ships.
void test_wifi_patch_request_schema_requires_ssid(void)
{
    cJSON *schema = cJSON_Parse(k_wifi_patch_request_schema);
    TEST_ASSERT_NOT_NULL_MESSAGE(schema,
        "PATCH /api/wifi request_schema must parse as valid JSON");

    cJSON *required = cJSON_GetObjectItem(schema, "required");
    TEST_ASSERT_NOT_NULL_MESSAGE(required,
        "PATCH /api/wifi request_schema must have a 'required' array");
    TEST_ASSERT_TRUE_MESSAGE(cJSON_IsArray(required),
        "PATCH /api/wifi request_schema 'required' must be an array");

    bool has_ssid = false;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, required) {
        if (cJSON_IsString(item) && strcmp(item->valuestring, "ssid") == 0) {
            has_ssid = true;
            break;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(has_ssid,
        "PATCH /api/wifi request_schema must list 'ssid' in required[]");

    cJSON_Delete(schema);
}

// Guard: PATCH /api/wifi route descriptor sets request_schema (OpenAPI requestBody
// will be emitted). Catches the regression where the field is accidentally cleared.
void test_wifi_patch_route_descriptor_has_request_schema(void)
{
    TEST_ASSERT_NOT_NULL_MESSAGE(s_wifi_patch_route.request_schema,
        "PATCH /api/wifi route descriptor must set request_schema");
    TEST_ASSERT_NOT_NULL_MESSAGE(s_wifi_patch_route.request_content_type,
        "PATCH /api/wifi route descriptor must set request_content_type");
}

// B1-413: PATCH /api/telemetry request_schema parses and has non-empty properties.
void test_telemetry_patch_request_schema_has_properties(void)
{
    TEST_ASSERT_TRUE_MESSAGE(
        check_request_schema_has_properties(k_telemetry_patch_request_schema),
        "PATCH /api/telemetry request_schema must parse and declare non-empty properties");
}

// B1-413: PATCH /api/sensors request_schema parses and has non-empty properties.
void test_sensors_patch_request_schema_has_properties(void)
{
    TEST_ASSERT_TRUE_MESSAGE(
        check_request_schema_has_properties(k_sensors_patch_request_schema),
        "PATCH /api/sensors request_schema must parse and declare non-empty properties");
}

// Guard: the bare-object regression — a PATCH route with request_schema but no properties
// — trips the walker.
void test_mutating_route_bare_body_guard_trips(void)
{
    static const bb_route_response_t bare_responses[] = {
        { 204, NULL, NULL, "ok" },
        { 0 },
    };
    static const bb_route_t bare_route = {
        .method               = BB_HTTP_PATCH,
        .path                 = "/api/bare-test",
        .request_content_type = "application/json",
        .request_schema       = "{\"type\":\"object\"}",  // no properties — bare object
        .responses            = bare_responses,
        .handler              = NULL,
    };

    bb_http_route_registry_clear();
    TEST_ASSERT_EQUAL(BB_OK, bb_http_register_route_descriptor_only(&bare_route));

    schema_check_ctx_t ctx = { 0 };
    bb_http_route_registry_foreach(route_schema_walker, &ctx);

    TEST_ASSERT_EQUAL_INT(1, ctx.mutating_bare_body);

    bb_http_route_registry_clear();
}

// ---------------------------------------------------------------------------
// Durable descriptor-fidelity guard (B1-246)
//
// Maintenance contract: every route with declared params, request body, or
// non-trivial response content types must have an entry in k_desc_audit[].
// When a new route is added that fits those criteria, add an entry — the
// "unknown route coverage" test below will NOT trip (it only fires if an
// entry in the table no longer matches any registered route). Adding a new
// route without updating this table means no param/content-type guard for it.
//
// Each entry specifies:
//   route   - path string (for diagnostics)
//   method  - BB_HTTP_* constant
//   want_params_named - expected query param names (NULL-terminated array)
//   want_request_schema_present - true iff request_schema must be non-NULL
//   want_response_content_type - expected content_type for the given status code
//   want_response_status - status code to check
//
// ---------------------------------------------------------------------------

typedef struct {
    const char  *route;
    bb_http_method_t method;
    // Params: NULL-terminated list of expected query param names.
    const char  *want_params_named[8];
    // Request body: true → request_schema must be non-NULL and non-empty.
    bool         want_request_schema;
    // Response content-type check: status=0 disables this check.
    int          want_response_status;
    const char  *want_response_content_type;
} desc_audit_entry_t;

// Mirrors production descriptors fixed in B1-246. Keep in sync with handler files.
// Null-terminated param lists; NULL at index 0 means no params expected.
static const desc_audit_entry_t k_desc_audit[] = {
    // Tier 1: params added
    { "/api/diag/coredump", BB_HTTP_GET,
      { "consume", NULL }, false, 500, "application/json" },
    { "/api/diag/heap",     BB_HTTP_GET,
      { "check", NULL },   false,   0, NULL },
    // Tier 1: content-type fixes
    { "/api/update/push",   BB_HTTP_POST,
      { NULL },             false, 200, "application/json" },
    { "/api/update/push",   BB_HTTP_POST,
      { NULL },             false, 400, "application/json" },
    { "/api/update/push",   BB_HTTP_POST,
      { NULL },             false, 500, "application/json" },
    { "/api/log/level",     BB_HTTP_POST,
      { NULL },             true,  400, "application/json" },
    // Tier 2: 500 responses added
    { "/api/events",            BB_HTTP_GET,
      { NULL },             false, 500, "application/json" },
    { "/api/update/apply",  BB_HTTP_POST,
      { NULL },             false, 500, "application/json" },
    { "/api/update/mark-valid", BB_HTTP_POST,
      { NULL },             false, 500, "application/json" },
    { "/api/diag/tasks",    BB_HTTP_GET,
      { NULL },             false, 500, "application/json" },
    { "/api/manifest",      BB_HTTP_GET,
      { NULL },             false, 500, "application/json" },
    // B1-413: request body schemas
    { "/api/telemetry", BB_HTTP_PATCH,
      { NULL },             true,    0, NULL },
    { "/api/sensors",   BB_HTTP_PATCH,
      { NULL },             true,    0, NULL },
    // Sentinel
    { NULL },
};

// Fixtures: minimal descriptors that mirror the production route descriptors
// for the routes in k_desc_audit. These are seeded into the registry so the
// walker can find them. Copy-pasted from production — edits to production
// literals must update these too.

// GET /api/diag/coredump fixture
static const bb_route_param_t s_coredump_params_fixture[] = {
    { "consume", "query", "erase coredump after full successful transfer", false, "string" },
};
static const bb_route_response_t s_coredump_responses_fixture[] = {
    { 200, "application/octet-stream", NULL, "raw coredump bytes" },
    { 404, "application/json",
      "{\"type\":\"object\",\"properties\":{\"error\":{\"type\":\"string\"}},\"required\":[\"error\"]}",
      "no coredump" },
    { 500, "application/json",
      "{\"type\":\"object\",\"properties\":{\"error\":{\"type\":\"string\"}},\"required\":[\"error\"]}",
      "alloc or partition failure" },
    { 0 },
};
static const bb_route_t s_coredump_route_fixture = {
    .method           = BB_HTTP_GET,
    .path             = "/api/diag/coredump",
    .tag              = "diag",
    .responses        = s_coredump_responses_fixture,
    .parameters       = s_coredump_params_fixture,
    .parameters_count = 1,
    .handler          = NULL,
};

// GET /api/diag/heap fixture
static const bb_route_param_t s_heap_params_fixture[] = {
    { "check", "query", "run heap integrity check", false, "string" },
};
static const bb_route_response_t s_heap_responses_fixture[] = {
    { 200, "application/json",
      "{\"type\":\"object\","
      "\"properties\":{\"integrity_ok\":{\"type\":\"boolean\"}},"
      "\"additionalProperties\":{\"type\":\"object\"}}",
      "heap stats" },
    { 0 },
};
static const bb_route_t s_heap_route_fixture = {
    .method           = BB_HTTP_GET,
    .path             = "/api/diag/heap",
    .tag              = "diag",
    .responses        = s_heap_responses_fixture,
    .parameters       = s_heap_params_fixture,
    .parameters_count = 1,
    .handler          = NULL,
};

// POST /api/update/push fixture
static const bb_route_response_t s_ota_push_responses_fixture[] = {
    { 200, "application/json",
      "{\"type\":\"object\",\"properties\":{\"status\":{\"type\":\"string\"}},\"required\":[\"status\"]}",
      "OTA complete" },
    { 400, "application/json",
      "{\"type\":\"object\",\"properties\":{\"error\":{\"type\":\"string\"}},\"required\":[\"error\"]}",
      "board mismatch" },
    { 408, "application/json",
      "{\"type\":\"object\",\"properties\":{\"error\":{\"type\":\"string\"}},\"required\":[\"error\"]}",
      "timeout" },
    { 413, "application/json",
      "{\"type\":\"object\",\"properties\":{\"error\":{\"type\":\"string\"}},\"required\":[\"error\"]}",
      "too large" },
    { 500, "application/json",
      "{\"type\":\"object\",\"properties\":{\"error\":{\"type\":\"string\"}},\"required\":[\"error\"]}",
      "write failed" },
    { 0 },
};
static const bb_route_t s_ota_push_route_fixture = {
    .method               = BB_HTTP_POST,
    .path                 = "/api/update/push",
    .tag                  = "update",
    .request_content_type = "application/octet-stream",
    .request_schema       = NULL,
    .responses            = s_ota_push_responses_fixture,
    .handler              = NULL,
};

// POST /api/log/level fixture
static const char k_log_level_post_req_schema[] =
    "{\"type\":\"object\","
    "\"properties\":{"
    "\"tag\":{\"type\":\"string\"},"
    "\"level\":{\"type\":\"string\","
    "\"enum\":[\"none\",\"error\",\"warn\",\"info\",\"debug\",\"verbose\"]}},"
    "\"required\":[\"tag\",\"level\"]}";
static const bb_route_response_t s_log_level_post_responses_fixture[] = {
    { 204, NULL, NULL, "updated" },
    { 400, "application/json",
      "{\"type\":\"object\",\"properties\":{\"error\":{\"type\":\"string\"}},\"required\":[\"error\"]}",
      "invalid tag or level" },
    { 0 },
};
static const bb_route_t s_log_level_post_route_fixture = {
    .method               = BB_HTTP_POST,
    .path                 = "/api/log/level",
    .tag                  = "logs",
    .request_content_type = "application/x-www-form-urlencoded",
    .request_schema       = k_log_level_post_req_schema,
    .responses            = s_log_level_post_responses_fixture,
    .handler              = NULL,
};

// GET /api/events fixture
static const bb_route_response_t s_events_responses_fixture[] = {
    { 200, "text/event-stream", NULL, "SSE stream" },
    { 500, "application/json",
      "{\"type\":\"object\",\"properties\":{\"error\":{\"type\":\"string\"}},\"required\":[\"error\"]}",
      "not initialized or async init failed" },
    { 503, "application/json",
      "{\"type\":\"object\","
      "\"properties\":{\"error\":{\"type\":\"string\",\"enum\":[\"max_clients\"]}},"
      "\"required\":[\"error\"]}",
      "max clients" },
    { 0 },
};
static const bb_route_t s_events_route_fixture = {
    .method    = BB_HTTP_GET,
    .path      = "/api/events",
    .tag       = "events",
    .responses = s_events_responses_fixture,
    .handler   = NULL,
};

// POST /api/update/apply fixture
static const bb_route_response_t s_ota_update_responses_fixture[] = {
    { 202, "application/json",
      "{\"type\":\"object\",\"properties\":{\"status\":{\"type\":\"string\"}},\"required\":[\"status\"]}",
      "started" },
    { 200, "application/json",
      "{\"type\":\"object\",\"properties\":{\"status\":{\"type\":\"string\"}},\"required\":[\"status\"]}",
      "up to date" },
    { 409, "application/json",
      "{\"type\":\"object\",\"properties\":{\"error\":{\"type\":\"string\"}},\"required\":[\"error\"]}",
      "in progress or no update" },
    { 500, "application/json",
      "{\"type\":\"object\",\"properties\":{\"error\":{\"type\":\"string\"}},\"required\":[\"error\"]}",
      "alloc or task create failed" },
    { 503, "application/json",
      "{\"type\":\"object\",\"properties\":{\"error\":{\"type\":\"string\"}},\"required\":[\"error\"]}",
      "no recent check" },
    { 0 },
};
static const bb_route_t s_ota_update_route_fixture = {
    .method    = BB_HTTP_POST,
    .path      = "/api/update/apply",
    .tag       = "update",
    .responses = s_ota_update_responses_fixture,
    .handler   = NULL,
};

// POST /api/update/mark-valid fixture
static const bb_route_response_t s_mark_valid_responses_fixture[] = {
    { 200, "application/json",
      "{\"type\":\"object\",\"properties\":{\"status\":{\"type\":\"string\"}},\"required\":[\"status\"]}",
      "marked valid" },
    { 409, "application/json",
      "{\"type\":\"object\",\"properties\":{\"error\":{\"type\":\"string\"}},\"required\":[\"error\"]}",
      "not pending" },
    { 500, "application/json",
      "{\"type\":\"object\",\"properties\":{\"error\":{\"type\":\"string\"}},\"required\":[\"error\"]}",
      "internal error" },
    { 0 },
};
static const bb_route_t s_mark_valid_route_fixture = {
    .method    = BB_HTTP_POST,
    .path      = "/api/update/mark-valid",
    .tag       = "update",
    .responses = s_mark_valid_responses_fixture,
    .handler   = NULL,
};

// GET /api/diag/tasks fixture
static const bb_route_response_t s_tasks_responses_fixture[] = {
    { 200, "application/json",
      "{\"type\":\"array\","
      "\"items\":{\"type\":\"object\","
      "\"properties\":{"
      "\"name\":{\"type\":\"string\"},"
      "\"prio\":{\"type\":\"integer\"},"
      "\"state\":{\"type\":\"string\"}}}}",
      "task list" },
    { 500, "application/json",
      "{\"type\":\"object\",\"properties\":{\"error\":{\"type\":\"string\"}},\"required\":[\"error\"]}",
      "alloc failed" },
    { 0 },
};
static const bb_route_t s_tasks_route_fixture = {
    .method    = BB_HTTP_GET,
    .path      = "/api/diag/tasks",
    .tag       = "diag",
    .responses = s_tasks_responses_fixture,
    .handler   = NULL,
};

// GET /api/manifest fixture (extends the existing k_manifest_schema fixture)
static const bb_route_response_t s_manifest_with_500_responses_fixture[] = {
    { 200, "application/json", k_manifest_schema, "manifest" },
    { 500, "application/json",
      "{\"type\":\"object\",\"properties\":{\"error\":{\"type\":\"string\"}},\"required\":[\"error\"]}",
      "emit or serialize failed" },
    { 0 },
};
static const bb_route_t s_manifest_with_500_route_fixture = {
    .method    = BB_HTTP_GET,
    .path      = "/api/manifest",
    .tag       = "manifest",
    .responses = s_manifest_with_500_responses_fixture,
    .handler   = NULL,
};


// ---------------------------------------------------------------------------
// Walker helpers for descriptor-audit assertions
// ---------------------------------------------------------------------------

// Find a registered route by (path, method). Returns the first matching entry.
typedef struct { const char *path; bb_http_method_t method; const bb_route_t *found; } find_ctx_t;

static void find_route_walker(const bb_route_t *r, void *ctx_)
{
    find_ctx_t *ctx = (find_ctx_t *)ctx_;
    if (!r || ctx->found) return;
    if (r->method == ctx->method && r->path && strcmp(r->path, ctx->path) == 0) {
        ctx->found = r;
    }
}

// Find a response entry by status code.
static const bb_route_response_t *find_response(const bb_route_t *r, int status)
{
    if (!r || !r->responses) return NULL;
    for (const bb_route_response_t *resp = r->responses; resp->status != 0; resp++) {
        if (resp->status == status) return resp;
    }
    return NULL;
}

// Check whether a route has a param with the given name in 'query'.
static bool has_query_param(const bb_route_t *r, const char *name)
{
    if (!r || !r->parameters || r->parameters_count == 0) return false;
    for (size_t i = 0; i < r->parameters_count; i++) {
        const bb_route_param_t *p = &r->parameters[i];
        if (p->name && strcmp(p->name, name) == 0 &&
            p->in   && strcmp(p->in,   "query") == 0) {
            return true;
        }
    }
    return false;
}

// Seed all B1-246 fixture routes into the registry, run the audit, then clear.
static void seed_desc_audit_fixtures(void)
{
    bb_http_route_registry_clear();
    bb_http_register_route_descriptor_only(&s_coredump_route_fixture);
    bb_http_register_route_descriptor_only(&s_heap_route_fixture);
    bb_http_register_route_descriptor_only(&s_ota_push_route_fixture);
    bb_http_register_route_descriptor_only(&s_log_level_post_route_fixture);
    bb_http_register_route_descriptor_only(&s_events_route_fixture);
    bb_http_register_route_descriptor_only(&s_ota_update_route_fixture);
    bb_http_register_route_descriptor_only(&s_mark_valid_route_fixture);
    bb_http_register_route_descriptor_only(&s_tasks_route_fixture);
    bb_http_register_route_descriptor_only(&s_manifest_with_500_route_fixture);
    bb_http_register_route_descriptor_only(&s_telemetry_patch_route_fixture);
    bb_http_register_route_descriptor_only(&s_sensors_patch_route_fixture);
}

// ---------------------------------------------------------------------------
// Descriptor-audit tests
// ---------------------------------------------------------------------------

// Every entry in k_desc_audit[] must find a registered route matching (path, method).
// If a fixture above is removed or renamed this fires.
void test_desc_audit_all_routes_registered(void)
{
    seed_desc_audit_fixtures();

    char msg[256];
    for (const desc_audit_entry_t *e = k_desc_audit; e->route != NULL; e++) {
        find_ctx_t ctx = { e->route, e->method, NULL };
        bb_http_route_registry_foreach(find_route_walker, &ctx);
        snprintf(msg, sizeof(msg),
                 "B1-246 audit: route not found in registry: %s (method %d)",
                 e->route, (int)e->method);
        TEST_ASSERT_NOT_NULL_MESSAGE(ctx.found, msg);
    }

    bb_http_route_registry_clear();
}

// For each k_desc_audit entry that specifies expected query params, verify the
// found route declares them.
void test_desc_audit_query_params(void)
{
    seed_desc_audit_fixtures();

    char msg[256];
    for (const desc_audit_entry_t *e = k_desc_audit; e->route != NULL; e++) {
        if (e->want_params_named[0] == NULL) continue;  // no params expected

        find_ctx_t ctx = { e->route, e->method, NULL };
        bb_http_route_registry_foreach(find_route_walker, &ctx);
        if (!ctx.found) continue;  // caught by test_desc_audit_all_routes_registered

        for (int i = 0; i < 8 && e->want_params_named[i] != NULL; i++) {
            snprintf(msg, sizeof(msg),
                     "B1-246 audit: %s (method %d) missing query param '%s'",
                     e->route, (int)e->method, e->want_params_named[i]);
            TEST_ASSERT_TRUE_MESSAGE(has_query_param(ctx.found, e->want_params_named[i]), msg);
        }
    }

    bb_http_route_registry_clear();
}

// For each k_desc_audit entry that requests request_schema presence, verify it.
void test_desc_audit_request_schema_presence(void)
{
    seed_desc_audit_fixtures();

    char msg[256];
    for (const desc_audit_entry_t *e = k_desc_audit; e->route != NULL; e++) {
        if (!e->want_request_schema) continue;

        find_ctx_t ctx = { e->route, e->method, NULL };
        bb_http_route_registry_foreach(find_route_walker, &ctx);
        if (!ctx.found) continue;

        snprintf(msg, sizeof(msg),
                 "B1-246 audit: %s (method %d) must have non-NULL request_schema",
                 e->route, (int)e->method);
        TEST_ASSERT_NOT_NULL_MESSAGE(ctx.found->request_schema, msg);
    }

    bb_http_route_registry_clear();
}

// For each k_desc_audit entry with a response content-type expectation, verify
// the declared content_type for that status code matches.
void test_desc_audit_response_content_types(void)
{
    seed_desc_audit_fixtures();

    char msg[256];
    for (const desc_audit_entry_t *e = k_desc_audit; e->route != NULL; e++) {
        if (e->want_response_status == 0 || e->want_response_content_type == NULL) continue;

        find_ctx_t ctx = { e->route, e->method, NULL };
        bb_http_route_registry_foreach(find_route_walker, &ctx);
        if (!ctx.found) continue;

        const bb_route_response_t *resp = find_response(ctx.found, e->want_response_status);
        snprintf(msg, sizeof(msg),
                 "B1-246 audit: %s (method %d) has no response entry for status %d",
                 e->route, (int)e->method, e->want_response_status);
        TEST_ASSERT_NOT_NULL_MESSAGE(resp, msg);

        snprintf(msg, sizeof(msg),
                 "B1-246 audit: %s status %d: expected content_type '%s' got '%s'",
                 e->route, e->want_response_status,
                 e->want_response_content_type,
                 resp->content_type ? resp->content_type : "(null)");
        TEST_ASSERT_NOT_NULL_MESSAGE(resp->content_type, msg);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(e->want_response_content_type,
                                         resp->content_type, msg);
    }

    bb_http_route_registry_clear();
}
