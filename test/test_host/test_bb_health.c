#include "unity.h"
#include "bb_health.h"
#include "bb_health_test.h"
#include "bb_health_section.h"

#include "cJSON.h"

#include <stddef.h>
#include <string.h>
#include <stdio.h>

#include "../../components/bb_health/bb_health_schema_priv.h"
#include "../../platform/host/bb_wifi/bb_wifi_test.h"

// ---------------------------------------------------------------------------
// Fixture: a tiny snap_desc/fill for schema-assembly tests, registered
// through the bb_health_section composer registry (the LIVE registry the
// handler renders from as of B1-1100 -- the legacy bb_response-backed
// bb_health_register_section() this file used to exercise is retired).
// ---------------------------------------------------------------------------

typedef struct {
    int64_t n;
} probe_snap_t;

static const bb_serialize_field_t s_probe_fields[] = {
    { .key = "n", .type = BB_TYPE_I64, .offset = offsetof(probe_snap_t, n) },
};

static const bb_serialize_desc_t s_probe_desc = {
    .type_name = "probe_snap_t",
    .fields    = s_probe_fields,
    .n_fields  = 1,
    .snap_size = sizeof(probe_snap_t),
};

static bb_err_t probe_fill(void *dst, const bb_health_fill_args_t *args)
{
    (void)args;
    ((probe_snap_t *)dst)->n = 1;
    return BB_OK;
}

// ---------------------------------------------------------------------------
// Schema assembly tests
// ---------------------------------------------------------------------------

void test_bb_health_assembled_schema_no_sections_equals_base_plus_suffix(void)
{
    const char *schema = bb_health_get_assembled_schema();
    TEST_ASSERT_NOT_NULL(schema);

    char expected[4096];
    snprintf(expected, sizeof(expected), "%s%s",
             k_health_base, k_health_suffix);
    TEST_ASSERT_EQUAL_STRING(expected, schema);
}

void test_bb_health_assembled_schema_is_valid_json(void)
{
    const char *schema = bb_health_get_assembled_schema();
    TEST_ASSERT_NOT_NULL(schema);
    cJSON *parsed = cJSON_Parse(schema);
    TEST_ASSERT_NOT_NULL_MESSAGE(parsed, "assembled health schema is not valid JSON");
    cJSON_Delete(parsed);
}

void test_bb_health_assembled_schema_contains_section_props(void)
{
    static const char sec_schema[] =
        "{\"type\":\"object\",\"properties\":{\"foo\":{\"type\":\"string\"}}}";
    bb_health_section_t sec = {
        .name = "mysec", .snap_desc = &s_probe_desc, .fill = probe_fill, .schema_props = sec_schema,
    };
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_health_section_register(&sec));
    const char *schema = bb_health_get_assembled_schema();
    TEST_ASSERT_NOT_NULL(schema);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(schema, "\"mysec\""),
                                 "section key not found in assembled health schema");
    TEST_ASSERT_NOT_NULL(strstr(schema, "\"foo\""));
}

void test_bb_health_assembled_schema_with_section_is_valid_json(void)
{
    static const char sec_schema[] =
        "{\"type\":\"object\",\"properties\":{\"present\":{\"type\":\"boolean\"}}}";
    bb_health_section_t sec = {
        .name = "mysec", .snap_desc = &s_probe_desc, .fill = probe_fill, .schema_props = sec_schema,
    };
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_health_section_register(&sec));
    const char *schema = bb_health_get_assembled_schema();
    TEST_ASSERT_NOT_NULL(schema);
    cJSON *parsed = cJSON_Parse(schema);
    TEST_ASSERT_NOT_NULL_MESSAGE(parsed, "health schema with section is not valid JSON");
    cJSON_Delete(parsed);
}

static void *failing_malloc(size_t sz)
{
    (void)sz;
    return NULL;
}

void test_bb_health_assembled_schema_oom_returns_null(void)
{
    bb_health_schema_set_malloc(failing_malloc);
    const char *schema = bb_health_get_assembled_schema();
    bb_health_schema_set_malloc(NULL);
    TEST_ASSERT_NULL(schema);
}

void test_bb_health_assembled_schema_repeated_calls_do_not_reassemble(void)
{
    // Mirrors the platform/espidf bb_health_init() re-entrancy guard
    // (B1-1096 review): the schema is assembled exactly ONCE and cached
    // (bb_health_host.c's s_assembled_schema lazy cache) -- a second call
    // (standing in for a WiFi-flap re-fire of the equivalent init path)
    // must return the SAME pointer, not a freshly-malloced string, or the
    // previous allocation would leak on every re-entry.
    const char *first  = bb_health_get_assembled_schema();
    const char *second = bb_health_get_assembled_schema();
    TEST_ASSERT_NOT_NULL(first);
    TEST_ASSERT_EQUAL_PTR(first, second);
}

void test_bb_health_assembled_schema_section_comma_is_valid_json(void)
{
    // Root fields are in the base; a section after them must have a leading
    // comma. k_health_base always ends with non-'{' content, so
    // bb_health_assemble_schema() always prepends one.
    static const char sec_schema[] =
        "{\"type\":\"object\",\"properties\":{\"x\":{\"type\":\"integer\"}}}";
    bb_health_section_t sec = {
        .name = "extra", .snap_desc = &s_probe_desc, .fill = probe_fill, .schema_props = sec_schema,
    };
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_health_section_register(&sec));
    const char *schema = bb_health_get_assembled_schema();
    TEST_ASSERT_NOT_NULL(schema);
    cJSON *parsed = cJSON_Parse(schema);
    TEST_ASSERT_NOT_NULL_MESSAGE(parsed,
        "health schema with section after root fields is not valid JSON (comma fix)");
    cJSON_Delete(parsed);
}

// ---------------------------------------------------------------------------
// bb_health_compute_ok tests
// ---------------------------------------------------------------------------

// On the host stub, bb_wifi_has_ip() returns false by default, so
// bb_health_compute_ok() must be false. ota_validated dropped (B1-977,
// bb_board dissolution) -- the gate is wifi-IP-only now.
void test_bb_health_compute_ok_false_on_host(void)
{
    bb_wifi_test_set_has_ip(false);
    TEST_ASSERT_FALSE(bb_health_compute_ok());
}

// wifi has IP → ok.
void test_bb_health_compute_ok_true_when_has_ip(void)
{
    bb_wifi_test_set_has_ip(true);
    TEST_ASSERT_TRUE(bb_health_compute_ok());
    // reset to default state
    bb_wifi_test_set_has_ip(false);
}

// wifi has no IP → not ok.
void test_bb_health_compute_ok_false_when_no_ip(void)
{
    bb_wifi_test_set_has_ip(false);
    TEST_ASSERT_FALSE(bb_health_compute_ok());
}

// ---------------------------------------------------------------------------
// Schema network field coverage tests
// ---------------------------------------------------------------------------

// The assembled schema must include ssid/bssid/ip/disc_reason (additive) and
// mdns (kept per locked decision B1-269).
void test_bb_health_schema_network_has_ssid(void)
{
    const char *schema = bb_health_get_assembled_schema();
    TEST_ASSERT_NOT_NULL(schema);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(schema, "\"ssid\""),
                                 "ssid missing from health schema network");
}

void test_bb_health_schema_network_has_bssid(void)
{
    const char *schema = bb_health_get_assembled_schema();
    TEST_ASSERT_NOT_NULL(schema);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(schema, "\"bssid\""),
                                 "bssid missing from health schema network");
}

void test_bb_health_schema_network_has_ip(void)
{
    const char *schema = bb_health_get_assembled_schema();
    TEST_ASSERT_NOT_NULL(schema);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(schema, "\"ip\""),
                                 "ip missing from health schema network");
}

// disc_reason is a numeric field relocated to /api/diag/net (TA-505).
void test_bb_health_schema_network_no_disc_reason(void)
{
    const char *schema = bb_health_get_assembled_schema();
    TEST_ASSERT_NOT_NULL(schema);
    TEST_ASSERT_NULL_MESSAGE(strstr(schema, "\"disc_reason\""),
                             "disc_reason must not be in health schema (TA-505: moved to /api/diag/net)");
}

// free_heap is a numeric field relocated to /api/diag/net (TA-505).
void test_bb_health_schema_no_free_heap(void)
{
    const char *schema = bb_health_get_assembled_schema();
    TEST_ASSERT_NOT_NULL(schema);
    TEST_ASSERT_NULL_MESSAGE(strstr(schema, "\"free_heap\""),
                             "free_heap must not be in health schema (TA-505: status bools only)");
}

// rssi is a numeric field relocated to /api/diag/net (TA-505).
void test_bb_health_schema_network_no_rssi(void)
{
    const char *schema = bb_health_get_assembled_schema();
    TEST_ASSERT_NOT_NULL(schema);
    TEST_ASSERT_NULL_MESSAGE(strstr(schema, "\"rssi\""),
                             "rssi must not be in health schema (TA-505: moved to /api/diag/net)");
}

void test_bb_health_schema_network_has_mdns(void)
{
    const char *schema = bb_health_get_assembled_schema();
    TEST_ASSERT_NOT_NULL(schema);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(schema, "\"mdns\""),
                                 "mdns missing from health schema network (locked: keep field)");
}

// ---------------------------------------------------------------------------
// Freeze test
// ---------------------------------------------------------------------------

void test_bb_health_register_section_after_freeze_returns_invalid_state(void)
{
    bb_health_freeze_for_test();
    bb_health_section_t sec = { .name = "x", .snap_desc = &s_probe_desc, .fill = probe_fill };
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_STATE, bb_health_section_register(&sec));
}
