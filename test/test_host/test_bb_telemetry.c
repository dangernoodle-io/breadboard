// Tests for bb_telemetry section registry + get/patch dispatch, plus the
// folded GET /api/telemetry/metrics Prometheus endpoint (B1-295).
#include "unity.h"
#include "bb_telemetry.h"
#include "bb_nv.h"
#include "bb_pub.h"
#include "bb_http.h"
#include "bb_dispatch_api.h"
#include "../../platform/host/bb_http/include/bb_http_host.h"
#include "bb_json.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Simple section stubs
// ---------------------------------------------------------------------------

static int s_get_calls;
static int s_patch_calls;
static bb_err_t s_patch_rc;

static void stub_get(bb_json_t section, void *ctx)
{
    (void)ctx;
    s_get_calls++;
    bb_json_obj_set_string(section, "key", "val");
}

static bb_err_t stub_patch(bb_json_t patch, void *ctx)
{
    (void)patch; (void)ctx;
    s_patch_calls++;
    return s_patch_rc;
}

static void reset_all(void)
{
    bb_telemetry_reset_for_test();
    bb_nv_host_str_store_reset();
    s_get_calls   = 0;
    s_patch_calls = 0;
    s_patch_rc    = BB_OK;
}

// ---------------------------------------------------------------------------
// register_section: basic / error cases
// ---------------------------------------------------------------------------

void test_bb_telemetry_register_ok(void)
{
    reset_all();
    bb_err_t rc = bb_telemetry_register_section("foo", stub_get, stub_patch, NULL);
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);
}

void test_bb_telemetry_register_null_name_returns_invalid_arg(void)
{
    reset_all();
    bb_err_t rc = bb_telemetry_register_section(NULL, stub_get, stub_patch, NULL);
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, rc);
}

void test_bb_telemetry_register_null_get_returns_invalid_arg(void)
{
    reset_all();
    bb_err_t rc = bb_telemetry_register_section("foo", NULL, stub_patch, NULL);
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, rc);
}

void test_bb_telemetry_register_overflow_returns_no_space(void)
{
    static const char *k_names[] = { "s0","s1","s2","s3","s4","s5","s6","s7" };
    reset_all();
    // Fill to capacity. Kconfig default is 6; the native test build pins
    // CONFIG_BB_TELEMETRY_MAX_SECTIONS=4 (see platformio.ini) to keep this
    // overflow scenario cheap.
    for (int i = 0; i < CONFIG_BB_TELEMETRY_MAX_SECTIONS; i++) {
        bb_err_t rc = bb_telemetry_register_section(k_names[i], stub_get, NULL, NULL);
        TEST_ASSERT_EQUAL_INT(BB_OK, rc);
    }
    bb_err_t rc = bb_telemetry_register_section("over", stub_get, NULL, NULL);
    TEST_ASSERT_EQUAL_INT(BB_ERR_NO_SPACE, rc);
}

void test_bb_telemetry_register_readonly_null_patch_ok(void)
{
    reset_all();
    bb_err_t rc = bb_telemetry_register_section("ro", stub_get, NULL, NULL);
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);
}

// ---------------------------------------------------------------------------
// build_get: empty / one / two sections
// ---------------------------------------------------------------------------

void test_bb_telemetry_build_get_empty_registry(void)
{
    reset_all();
    bb_json_t root = bb_json_obj_new();
    bb_telemetry_build_get_for_test(root);
    char *s = bb_json_serialize(root);
    TEST_ASSERT_NOT_NULL(s);
    // Empty object.
    TEST_ASSERT_EQUAL_STRING("{}", s);
    bb_json_free_str(s);
    bb_json_free(root);
}

void test_bb_telemetry_build_get_one_section(void)
{
    reset_all();
    bb_telemetry_register_section("alpha", stub_get, NULL, NULL);
    bb_json_t root = bb_json_obj_new();
    bb_telemetry_build_get_for_test(root);

    TEST_ASSERT_EQUAL_INT(1, s_get_calls);

    bb_json_t alpha = bb_json_obj_get_item(root, "alpha");
    TEST_ASSERT_NOT_NULL(alpha);

    char val[32] = {0};
    bool ok = bb_json_obj_get_string(alpha, "key", val, sizeof(val));
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_STRING("val", val);

    bb_json_free(root);
}

void test_bb_telemetry_build_get_two_sections(void)
{
    reset_all();
    bb_telemetry_register_section("a", stub_get, NULL, NULL);
    bb_telemetry_register_section("b", stub_get, NULL, NULL);

    bb_json_t root = bb_json_obj_new();
    bb_telemetry_build_get_for_test(root);

    TEST_ASSERT_EQUAL_INT(2, s_get_calls);

    TEST_ASSERT_NOT_NULL(bb_json_obj_get_item(root, "a"));
    TEST_ASSERT_NOT_NULL(bb_json_obj_get_item(root, "b"));

    bb_json_free(root);
}

// ---------------------------------------------------------------------------
// dispatch_patch: correct section / unknown ignored / read-only → err
// ---------------------------------------------------------------------------

void test_bb_telemetry_dispatch_patch_known_section(void)
{
    reset_all();
    bb_telemetry_register_section("s", stub_get, stub_patch, NULL);

    bb_json_t body = bb_json_parse("{\"s\":{\"x\":1}}", 0);
    TEST_ASSERT_NOT_NULL(body);

    bb_err_t rc = bb_telemetry_dispatch_patch_for_test(body);
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);
    TEST_ASSERT_EQUAL_INT(1, s_patch_calls);

    bb_json_free(body);
}

void test_bb_telemetry_dispatch_patch_unknown_section_ignored(void)
{
    reset_all();
    bb_telemetry_register_section("s", stub_get, stub_patch, NULL);

    bb_json_t body = bb_json_parse("{\"unknown\":{\"x\":1}}", 0);
    TEST_ASSERT_NOT_NULL(body);

    bb_err_t rc = bb_telemetry_dispatch_patch_for_test(body);
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);
    TEST_ASSERT_EQUAL_INT(0, s_patch_calls);

    bb_json_free(body);
}

void test_bb_telemetry_dispatch_patch_readonly_returns_invalid_arg(void)
{
    reset_all();
    bb_telemetry_register_section("ro", stub_get, NULL /* read-only */, NULL);

    bb_json_t body = bb_json_parse("{\"ro\":{\"x\":1}}", 0);
    TEST_ASSERT_NOT_NULL(body);

    bb_err_t rc = bb_telemetry_dispatch_patch_for_test(body);
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, rc);

    bb_json_free(body);
}

// ---------------------------------------------------------------------------
// assemble_get_schema: real composed schema (not generic {type:object})
// ---------------------------------------------------------------------------

void test_bb_telemetry_assemble_get_schema_empty_is_object(void)
{
    reset_all();
    char *s = bb_telemetry_assemble_get_schema();
    TEST_ASSERT_NOT_NULL(s);
    // Should be a valid object schema with empty properties.
    TEST_ASSERT_NOT_NULL(strstr(s, "\"type\":\"object\""));
    TEST_ASSERT_NOT_NULL(strstr(s, "\"properties\":{"));
    free(s);
}

void test_bb_telemetry_assemble_get_schema_section_with_props_appears(void)
{
    reset_all();
    bb_telemetry_register_section_ex("mqtt", stub_get, NULL, NULL,
                                      "{\"type\":\"object\"}");
    char *s = bb_telemetry_assemble_get_schema();
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(s, "\"mqtt\""),
        "assembled schema must contain section name 'mqtt'");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(s, "{\"type\":\"object\"}"),
        "assembled schema must contain the section's schema_props");
    free(s);
}

void test_bb_telemetry_assemble_get_schema_no_props_section_omitted(void)
{
    reset_all();
    // Register without schema_props — section should not appear in schema.
    bb_telemetry_register_section("publisher", stub_get, NULL, NULL);
    char *s = bb_telemetry_assemble_get_schema();
    TEST_ASSERT_NOT_NULL(s);
    // Properties block contains only the root-level pending_reboot field
    // (B1-462a) — no sections, since "publisher" declared no schema_props.
    TEST_ASSERT_EQUAL_STRING(
        "{\"type\":\"object\",\"properties\":{"
        "\"pending_reboot\":{\"type\":\"boolean\"}}}", s);
    free(s);
}

// B1-462a: /api/telemetry emits a top-level pending_reboot field (see
// telemetry_get_handler in bb_telemetry_routes.c) that is not a registered
// section; the composed GET schema must declare it so schema==emit.
void test_bb_telemetry_assemble_get_schema_declares_pending_reboot(void)
{
    reset_all();
    char *s = bb_telemetry_assemble_get_schema();
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(s, "\"pending_reboot\":{\"type\":\"boolean\"}"),
        "assembled schema must declare pending_reboot");
    free(s);
}

// ===========================================================================
// GET /api/telemetry/metrics — four param combos + bb_pub accessor coverage (B1-295)
// ===========================================================================

// Source A: emits two numbers (a=10, b=20)
static bool m_src_numbers(bb_json_t obj, void *ctx)
{
    (void)ctx;
    bb_json_obj_set_number(obj, "a", 10.0);
    bb_json_obj_set_number(obj, "b", 20.0);
    return true;
}

// Source B: emits one string field
static bool m_src_string(bb_json_t obj, void *ctx)
{
    (void)ctx;
    bb_json_obj_set_string(obj, "version", "1.2.3");
    return true;
}

// Source C: always returns false (should be skipped)
static bool m_src_skip(bb_json_t obj, void *ctx)
{
    (void)obj; (void)ctx;
    return false;
}

static void m_reset_all(void)
{
    bb_pub_test_reset();
    bb_nv_config_set_hostname("testhost");
    bb_http_route_registry_clear();
}

static void m_setup_with_sources(void)
{
    m_reset_all();
    bb_pub_register_source("nums",   m_src_numbers, NULL);
    bb_pub_register_source("strfld", m_src_string,  NULL);
    bb_pub_register_source("skipper", m_src_skip,   NULL);
    bb_telemetry_init(NULL);  // NULL server OK on host; registers /api/telemetry/metrics
}

typedef struct {
    int   status;
    char  content_type[64];
    char *body;
    size_t body_len;
} m_cap_t;

static void m_cap_free(m_cap_t *c) { free(c->body); c->body = NULL; }

static void m_run_handler_qs(const char *query_string, m_cap_t *out)
{
    bb_http_handler_fn handler = NULL;
    bb_dispatch_api_lookup(BB_HTTP_GET, "/api/telemetry/metrics", &handler);
    if (!handler) {
        bb_telemetry_init(NULL);
        bb_dispatch_api_lookup(BB_HTTP_GET, "/api/telemetry/metrics", &handler);
    }
    TEST_ASSERT_NOT_NULL_MESSAGE(handler, "metrics handler not registered");

    bb_http_request_t *req = NULL;
    bb_http_host_capture_begin(&req);
    if (query_string && *query_string) {
        bb_http_host_capture_set_query_string(query_string);
    }
    handler(req);
    bb_http_host_capture_t raw;
    bb_http_host_capture_end(req, &raw);
    out->status   = raw.status;
    out->body     = raw.body;
    out->body_len = raw.body_len;
    strncpy(out->content_type, raw.content_type, sizeof(out->content_type) - 1);
    out->content_type[sizeof(out->content_type) - 1] = '\0';
}

// ---------------------------------------------------------------------------
// bb_pub accessor tests
// ---------------------------------------------------------------------------

void test_bb_pub_source_count_returns_registered(void)
{
    m_reset_all();
    TEST_ASSERT_EQUAL_INT(0, bb_pub_source_count());
    bb_pub_register_source("a", m_src_numbers, NULL);
    TEST_ASSERT_EQUAL_INT(1, bb_pub_source_count());
    bb_pub_register_source("b", m_src_string, NULL);
    TEST_ASSERT_EQUAL_INT(2, bb_pub_source_count());
}

void test_bb_pub_source_info_out_of_range_returns_invalid_arg(void)
{
    m_reset_all();
    bb_err_t rc = bb_pub_source_info(0, NULL, NULL, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, rc);
}

void test_bb_pub_source_info_returns_correct_subtopic(void)
{
    m_reset_all();
    bb_pub_register_source("mysrc", m_src_numbers, NULL);
    const char *subtopic = NULL;
    bb_err_t rc = bb_pub_source_info(0, &subtopic, NULL, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);
    TEST_ASSERT_EQUAL_STRING("mysrc", subtopic);
}

void test_bb_pub_source_info_sampled_ever_starts_false(void)
{
    m_reset_all();
    bb_pub_register_source("s", m_src_numbers, NULL);
    bool sampled = true;
    uint32_t last_ms = 99;
    bb_pub_source_info(0, NULL, NULL, NULL, &last_ms, &sampled);
    TEST_ASSERT_FALSE(sampled);
    TEST_ASSERT_EQUAL_UINT32(0, last_ms);
}

void test_bb_pub_ring_undersized_false_by_default(void)
{
    m_reset_all();
    TEST_ASSERT_FALSE(bb_pub_ring_undersized());
}

// ---------------------------------------------------------------------------
// 1. GET /api/telemetry/metrics — Prometheus values (default)
// ---------------------------------------------------------------------------

void test_bb_metrics_prom_values_content_type(void)
{
    m_setup_with_sources();
    m_cap_t cap = {0};
    m_run_handler_qs("", &cap);
    TEST_ASSERT_EQUAL_INT(200, cap.status);
    TEST_ASSERT_NOT_NULL(cap.body);
    TEST_ASSERT_NOT_NULL(strstr(cap.content_type, "text/plain"));
    m_cap_free(&cap);
}

void test_bb_metrics_prom_values_contains_type_line(void)
{
    m_setup_with_sources();
    m_cap_t cap = {0};
    m_run_handler_qs("", &cap);
    TEST_ASSERT_NOT_NULL(cap.body);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(cap.body, "# TYPE"), "missing # TYPE line");
    m_cap_free(&cap);
}

void test_bb_metrics_prom_values_contains_numeric_metric(void)
{
    m_setup_with_sources();
    m_cap_t cap = {0};
    m_run_handler_qs("", &cap);
    TEST_ASSERT_NOT_NULL(cap.body);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(cap.body, "bb_nums_a{host=\"testhost\"}"),
                                  "missing bb_nums_a gauge line");
    m_cap_free(&cap);
}

void test_bb_metrics_prom_values_contains_b_metric(void)
{
    m_setup_with_sources();
    m_cap_t cap = {0};
    m_run_handler_qs("", &cap);
    TEST_ASSERT_NOT_NULL(cap.body);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(cap.body, "bb_nums_b{host=\"testhost\"}"),
                                  "missing bb_nums_b gauge line");
    m_cap_free(&cap);
}

void test_bb_metrics_prom_values_contains_info_metric(void)
{
    m_setup_with_sources();
    m_cap_t cap = {0};
    m_run_handler_qs("", &cap);
    TEST_ASSERT_NOT_NULL(cap.body);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(cap.body, "bb_strfld_info"),
                                  "missing bb_strfld_info metric");
    m_cap_free(&cap);
}

void test_bb_metrics_prom_values_info_has_label(void)
{
    m_setup_with_sources();
    m_cap_t cap = {0};
    m_run_handler_qs("", &cap);
    TEST_ASSERT_NOT_NULL(cap.body);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(cap.body, "version=\"1.2.3\""),
                                  "missing version label in info line");
    m_cap_free(&cap);
}

void test_bb_metrics_prom_values_skip_source_emits_nothing(void)
{
    m_setup_with_sources();
    m_cap_t cap = {0};
    m_run_handler_qs("", &cap);
    TEST_ASSERT_NOT_NULL(cap.body);
    TEST_ASSERT_NULL_MESSAGE(strstr(cap.body, "bb_skipper_"), "skipper source should not appear");
    m_cap_free(&cap);
}

void test_bb_metrics_prom_values_has_pub_ring_undersized(void)
{
    m_setup_with_sources();
    m_cap_t cap = {0};
    m_run_handler_qs("", &cap);
    TEST_ASSERT_NOT_NULL(cap.body);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(cap.body, "bb_pub_ring_undersized"),
                                  "missing bb_pub_ring_undersized gauge");
    m_cap_free(&cap);
}

void test_bb_metrics_prom_values_has_pub_buffer_dropped(void)
{
    m_setup_with_sources();
    m_cap_t cap = {0};
    m_run_handler_qs("", &cap);
    TEST_ASSERT_NOT_NULL(cap.body);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(cap.body, "bb_pub_buffer_dropped"),
                                  "missing bb_pub_buffer_dropped gauge");
    m_cap_free(&cap);
}

// ---------------------------------------------------------------------------
// 2. GET /api/telemetry/metrics?format=json — JSON values
// ---------------------------------------------------------------------------

void test_bb_metrics_json_values_content_type(void)
{
    m_setup_with_sources();
    m_cap_t cap = {0};
    m_run_handler_qs("format=json", &cap);
    TEST_ASSERT_EQUAL_INT(200, cap.status);
    TEST_ASSERT_NOT_NULL(cap.body);
    TEST_ASSERT_NOT_NULL(strstr(cap.content_type, "application/json"));
    m_cap_free(&cap);
}

void test_bb_metrics_json_values_has_sources_key(void)
{
    m_setup_with_sources();
    m_cap_t cap = {0};
    m_run_handler_qs("format=json", &cap);
    TEST_ASSERT_NOT_NULL(cap.body);
    bb_json_t doc = bb_json_parse(cap.body, 0);
    TEST_ASSERT_NOT_NULL_MESSAGE(doc, "failed to parse JSON body");
    bb_json_t sources = bb_json_obj_get_item(doc, "sources");
    TEST_ASSERT_NOT_NULL_MESSAGE(sources, "missing 'sources' key");
    bb_json_free(doc);
    m_cap_free(&cap);
}

void test_bb_metrics_json_values_has_nums_source(void)
{
    m_setup_with_sources();
    m_cap_t cap = {0};
    m_run_handler_qs("format=json", &cap);
    TEST_ASSERT_NOT_NULL(cap.body);
    bb_json_t doc = bb_json_parse(cap.body, 0);
    TEST_ASSERT_NOT_NULL(doc);
    bb_json_t sources = bb_json_obj_get_item(doc, "sources");
    TEST_ASSERT_NOT_NULL(sources);
    bb_json_t nums = bb_json_obj_get_item(sources, "nums");
    TEST_ASSERT_NOT_NULL_MESSAGE(nums, "missing 'nums' in sources");
    double a = 0.0;
    bool ok = bb_json_obj_get_number(nums, "a", &a);
    TEST_ASSERT_TRUE_MESSAGE(ok, "missing field 'a' in nums");
    TEST_ASSERT_EQUAL_DOUBLE(10.0, a);
    bb_json_free(doc);
    m_cap_free(&cap);
}

void test_bb_metrics_json_values_has_publisher_key(void)
{
    m_setup_with_sources();
    m_cap_t cap = {0};
    m_run_handler_qs("format=json", &cap);
    TEST_ASSERT_NOT_NULL(cap.body);
    bb_json_t doc = bb_json_parse(cap.body, 0);
    TEST_ASSERT_NOT_NULL(doc);
    bb_json_t pub = bb_json_obj_get_item(doc, "publisher");
    TEST_ASSERT_NOT_NULL_MESSAGE(pub, "missing 'publisher' key");
    bb_json_free(doc);
    m_cap_free(&cap);
}

void test_bb_metrics_json_values_skip_source_absent(void)
{
    m_setup_with_sources();
    m_cap_t cap = {0};
    m_run_handler_qs("format=json", &cap);
    TEST_ASSERT_NOT_NULL(cap.body);
    bb_json_t doc = bb_json_parse(cap.body, 0);
    TEST_ASSERT_NOT_NULL(doc);
    bb_json_t sources = bb_json_obj_get_item(doc, "sources");
    TEST_ASSERT_NOT_NULL(sources);
    bb_json_t skipper = bb_json_obj_get_item(sources, "skipper");
    TEST_ASSERT_NULL_MESSAGE(skipper, "skipper source must not appear in JSON");
    bb_json_free(doc);
    m_cap_free(&cap);
}

// ---------------------------------------------------------------------------
// 3. GET /api/telemetry/metrics?schema — Prometheus schema only
// ---------------------------------------------------------------------------

void test_bb_metrics_schema_prom_content_type(void)
{
    m_setup_with_sources();
    m_cap_t cap = {0};
    m_run_handler_qs("schema", &cap);
    TEST_ASSERT_EQUAL_INT(200, cap.status);
    TEST_ASSERT_NOT_NULL(cap.body);
    TEST_ASSERT_NOT_NULL(strstr(cap.content_type, "text/plain"));
    m_cap_free(&cap);
}

void test_bb_metrics_schema_prom_has_type_lines(void)
{
    m_setup_with_sources();
    m_cap_t cap = {0};
    m_run_handler_qs("schema", &cap);
    TEST_ASSERT_NOT_NULL(cap.body);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(cap.body, "# TYPE"), "missing # TYPE");
    m_cap_free(&cap);
}

void test_bb_metrics_schema_prom_no_value_lines(void)
{
    m_setup_with_sources();
    m_cap_t cap = {0};
    m_run_handler_qs("schema", &cap);
    TEST_ASSERT_NOT_NULL(cap.body);
    TEST_ASSERT_NULL_MESSAGE(strstr(cap.body, "} 10"),
                              "value line present in schema-only response");
    m_cap_free(&cap);
}

// ---------------------------------------------------------------------------
// 4. GET /api/telemetry/metrics?schema&format=json — JSON schema descriptor
// ---------------------------------------------------------------------------

void test_bb_metrics_schema_json_content_type(void)
{
    m_setup_with_sources();
    m_cap_t cap = {0};
    m_run_handler_qs("schema&format=json", &cap);
    TEST_ASSERT_EQUAL_INT(200, cap.status);
    TEST_ASSERT_NOT_NULL(cap.body);
    TEST_ASSERT_NOT_NULL(strstr(cap.content_type, "application/json"));
    m_cap_free(&cap);
}

void test_bb_metrics_schema_json_has_prefix(void)
{
    m_setup_with_sources();
    m_cap_t cap = {0};
    m_run_handler_qs("schema&format=json", &cap);
    TEST_ASSERT_NOT_NULL(cap.body);
    bb_json_t doc = bb_json_parse(cap.body, 0);
    TEST_ASSERT_NOT_NULL(doc);
    char pfx[32] = "";
    bb_json_obj_get_string(doc, "prefix", pfx, sizeof(pfx));
    TEST_ASSERT_EQUAL_STRING("bb", pfx);
    bb_json_free(doc);
    m_cap_free(&cap);
}

void test_bb_metrics_schema_json_has_metrics_array(void)
{
    m_setup_with_sources();
    m_cap_t cap = {0};
    m_run_handler_qs("schema&format=json", &cap);
    TEST_ASSERT_NOT_NULL(cap.body);
    bb_json_t doc = bb_json_parse(cap.body, 0);
    TEST_ASSERT_NOT_NULL(doc);
    bb_json_t metrics = bb_json_obj_get_item(doc, "metrics");
    TEST_ASSERT_NOT_NULL_MESSAGE(metrics, "missing 'metrics' array");
    TEST_ASSERT_TRUE_MESSAGE(bb_json_item_is_array(metrics), "'metrics' is not array");
    TEST_ASSERT_GREATER_THAN_MESSAGE(0, bb_json_arr_size(metrics), "metrics array empty");
    bb_json_free(doc);
    m_cap_free(&cap);
}

void test_bb_metrics_schema_json_has_publisher_array(void)
{
    m_setup_with_sources();
    m_cap_t cap = {0};
    m_run_handler_qs("schema&format=json", &cap);
    TEST_ASSERT_NOT_NULL(cap.body);
    bb_json_t doc = bb_json_parse(cap.body, 0);
    TEST_ASSERT_NOT_NULL(doc);
    bb_json_t pub = bb_json_obj_get_item(doc, "publisher");
    TEST_ASSERT_NOT_NULL_MESSAGE(pub, "missing 'publisher' array");
    TEST_ASSERT_TRUE_MESSAGE(bb_json_item_is_array(pub), "'publisher' is not array");
    bb_json_free(doc);
    m_cap_free(&cap);
}

void test_bb_metrics_schema_json_no_sources_key(void)
{
    m_setup_with_sources();
    m_cap_t cap = {0};
    m_run_handler_qs("schema&format=json", &cap);
    TEST_ASSERT_NOT_NULL(cap.body);
    bb_json_t doc = bb_json_parse(cap.body, 0);
    TEST_ASSERT_NOT_NULL(doc);
    bb_json_t src = bb_json_obj_get_item(doc, "sources");
    TEST_ASSERT_NULL_MESSAGE(src, "'sources' key should not appear in schema JSON");
    bb_json_free(doc);
    m_cap_free(&cap);
}

// ---------------------------------------------------------------------------
// 5. Custom prefix via bb_pub_set_metrics_prefix
// ---------------------------------------------------------------------------

void test_bb_metrics_set_prefix_changes_emitted_names(void)
{
    m_reset_all();
    bb_pub_register_source("nums", m_src_numbers, NULL);
    bb_pub_set_metrics_prefix("taipanminer");
    bb_telemetry_init(NULL);

    m_cap_t cap = {0};
    m_run_handler_qs("", &cap);
    TEST_ASSERT_NOT_NULL(cap.body);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(cap.body, "taipanminer_nums_a"),
                                  "custom prefix not reflected in output");
    TEST_ASSERT_NULL_MESSAGE(strstr(cap.body, "bb_nums_a"),
                              "old 'bb' prefix still in output");
    m_cap_free(&cap);
}

void test_bb_metrics_set_prefix_reflected_in_schema_json(void)
{
    m_reset_all();
    bb_pub_register_source("nums", m_src_numbers, NULL);
    bb_pub_set_metrics_prefix("taipanminer");
    bb_telemetry_init(NULL);

    m_cap_t cap = {0};
    m_run_handler_qs("schema&format=json", &cap);
    TEST_ASSERT_NOT_NULL(cap.body);
    bb_json_t doc = bb_json_parse(cap.body, 0);
    TEST_ASSERT_NOT_NULL(doc);
    char pfx[32] = "";
    bb_json_obj_get_string(doc, "prefix", pfx, sizeof(pfx));
    TEST_ASSERT_EQUAL_STRING("taipanminer", pfx);
    bb_json_free(doc);
    m_cap_free(&cap);
}
