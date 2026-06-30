#include "unity.h"
#include "bb_health.h"
#include "bb_health_test.h"
#include "bb_json.h"

#include "cJSON.h"

#include <string.h>
#include <stdio.h>

#include "../../components/bb_health/bb_health_schema_priv.h"
#include "../../platform/host/bb_wifi/bb_wifi_test.h"
#include "../../platform/host/bb_board/bb_board_test.h"

static void test_section_get_fn(bb_json_t section, void *ctx)
{
    (void)ctx;
    (void)section;
}

// ---------------------------------------------------------------------------
// bb_health section registration tests
// ---------------------------------------------------------------------------

void test_bb_health_register_section_null_name_returns_err(void)
{
    bb_err_t err = bb_health_register_section(NULL, test_section_get_fn, NULL, NULL);
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, err);
}

void test_bb_health_register_section_null_get_returns_err(void)
{
    bb_err_t err = bb_health_register_section("test", NULL, NULL, NULL);
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, err);
}

void test_bb_health_register_section_ok(void)
{
    bb_err_t err = bb_health_register_section("test", test_section_get_fn, NULL, NULL);
    TEST_ASSERT_EQUAL_INT(BB_OK, err);
}

void test_bb_health_register_section_capacity(void)
{
    static const char *k_names[] = { "s0","s1","s2","s3","s4","s5","s6","s7" };
    // Fill the table completely (BB_SECTION_MAX = 8 by default).
    for (int i = 0; i < BB_SECTION_MAX; i++) {
        bb_err_t err = bb_health_register_section(k_names[i], test_section_get_fn, NULL, NULL);
        TEST_ASSERT_EQUAL_INT(BB_OK, err);
    }
    // One over capacity should return NO_SPACE.
    bb_err_t err = bb_health_register_section("over", test_section_get_fn, NULL, NULL);
    TEST_ASSERT_EQUAL_INT(BB_ERR_NO_SPACE, err);
}

void test_bb_health_register_section_after_freeze_returns_invalid_state(void)
{
    bb_health_freeze_for_test();
    bb_err_t err = bb_health_register_section("x", test_section_get_fn, NULL, NULL);
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_STATE, err);
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
    bb_err_t err = bb_health_register_section("mysec", test_section_get_fn, NULL, sec_schema);
    TEST_ASSERT_EQUAL_INT(BB_OK, err);
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
    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_health_register_section("mysec", test_section_get_fn, NULL, sec_schema));
    const char *schema = bb_health_get_assembled_schema();
    TEST_ASSERT_NOT_NULL(schema);
    cJSON *parsed = cJSON_Parse(schema);
    TEST_ASSERT_NOT_NULL_MESSAGE(parsed, "health schema with section is not valid JSON");
    cJSON_Delete(parsed);
}

void test_bb_health_assembled_schema_section_comma_is_valid_json(void)
{
    // Root fields are in the base; a section after them must have a leading comma.
    // bb_section_assemble_schema handles this automatically (base ends with non-'{').
    static const char sec_schema[] =
        "{\"type\":\"object\",\"properties\":{\"x\":{\"type\":\"integer\"}}}";
    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_health_register_section("extra", test_section_get_fn, NULL, sec_schema));
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

// On the host stub, bb_wifi_has_ip() returns false and bb_board_get_info()
// returns ota_validated=false, so bb_health_compute_ok() must be false.
void test_bb_health_compute_ok_false_on_host(void)
{
    bb_wifi_test_set_has_ip(false);
    bb_board_test_set_ota_validated(false);
    TEST_ASSERT_FALSE(bb_health_compute_ok());
}

// When both conditions are met (wifi has IP AND board reports ota_validated),
// bb_health_compute_ok() must be true — reproduces the hardware-validation
// regression where health.ok was false despite /api/info showing validated:true.
void test_bb_health_compute_ok_true_when_wifi_and_board_validated(void)
{
    bb_wifi_test_set_has_ip(true);
    bb_board_test_set_ota_validated(true);
    TEST_ASSERT_TRUE(bb_health_compute_ok());
    // reset to default state
    bb_wifi_test_set_has_ip(false);
    bb_board_test_set_ota_validated(false);
}

// wifi has IP but board not yet validated → not ok.
void test_bb_health_compute_ok_false_when_not_validated(void)
{
    bb_wifi_test_set_has_ip(true);
    bb_board_test_set_ota_validated(false);
    TEST_ASSERT_FALSE(bb_health_compute_ok());
    bb_wifi_test_set_has_ip(false);
}

// board validated but wifi has no IP → not ok.
void test_bb_health_compute_ok_false_when_no_ip(void)
{
    bb_wifi_test_set_has_ip(false);
    bb_board_test_set_ota_validated(true);
    TEST_ASSERT_FALSE(bb_health_compute_ok());
    bb_board_test_set_ota_validated(false);
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
// Section invocation test
// ---------------------------------------------------------------------------

static bool s_invoked = false;
static void track_section_get(bb_json_t section, void *ctx)
{
    (void)ctx;
    s_invoked = true;
    bb_json_obj_set_bool(section, "alive", true);
}

void test_bb_health_section_get_fn_invoked(void)
{
    s_invoked = false;
    bb_health_register_section("probe", track_section_get, NULL, NULL);
    bb_json_t root = bb_json_obj_new();
    bb_health_invoke_sections_for_test(root);
    TEST_ASSERT_TRUE_MESSAGE(s_invoked, "section get_fn was not invoked");

    bb_json_t probe = bb_json_obj_get_item(root, "probe");
    TEST_ASSERT_NOT_NULL_MESSAGE(probe, "probe section missing from root after invoke");

    bool alive = false;
    TEST_ASSERT_TRUE(bb_json_obj_get_bool(probe, "alive", &alive));
    TEST_ASSERT_TRUE(alive);

    bb_json_free(root);
}
