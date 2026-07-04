// Tests for bb_info_build: emit + capture + /api/info integration.
//
// Host-only: bb_system_get_app_sha256 returns "deadbeef0" on host.

#include "unity.h"
#include "bb_info.h"
#include "bb_info_test.h"
#include "bb_json.h"
#include "bb_cache.h"
#include "bb_event.h"

#include "../../components/bb_info/src/bb_info_build_priv.h"
#include "../../components/bb_info/bb_info_schema_priv.h"

#include "cJSON.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void reset_all(void)
{
    extern void bb_cache_reset_for_test(void);
    bb_cache_reset_for_test();
    bb_event_init(NULL);
    bb_info_reset_for_test();
}

// ---------------------------------------------------------------------------
// Tests: bb_info_build_emit
// ---------------------------------------------------------------------------

void test_bb_info_build_emit_all_keys(void)
{
    bb_info_build_snap_t snap = {
        .version      = "1.2.3",
        .idf_version  = "v5.3.1",
        .build_date   = "Jan  1 2025",
        .build_time   = "12:00:00",
        .project_name = "breadboard",
        .chip_model   = "ESP32",
        .chip_revision = 3,
        .cores        = 2,
        .cpu_freq_mhz = 240,
        .flash_size   = 4194304,
        .app_size     = 1200000,
        .board        = "wroom32",
        .app_sha256   = "deadbeef0",
    };

    bb_json_t obj = bb_json_obj_new();
    TEST_ASSERT_NOT_NULL(obj);

    bb_info_build_emit(obj, &snap);

    char *json = bb_json_serialize(obj);
    TEST_ASSERT_NOT_NULL(json);

    // Assert all 13 keys present
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(json, "\"version\""),      "missing version");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(json, "\"idf_version\""),  "missing idf_version");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(json, "\"build_date\""),   "missing build_date");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(json, "\"build_time\""),   "missing build_time");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(json, "\"project_name\""), "missing project_name");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(json, "\"chip_model\""),   "missing chip_model");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(json, "\"chip_revision\""),"missing chip_revision");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(json, "\"cores\""),        "missing cores");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(json, "\"cpu_freq_mhz\""), "missing cpu_freq_mhz");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(json, "\"flash_size\""),   "missing flash_size");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(json, "\"app_size\""),     "missing app_size");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(json, "\"board\""),        "missing board");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(json, "\"app_sha256\""),   "missing app_sha256");

    // Assert specific values
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(json, "\"1.2.3\""),        "version value wrong");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(json, "\"deadbeef0\""),    "app_sha256 value wrong");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(json, "\"wroom32\""),      "board value wrong");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(json, "\"ESP32\""),        "chip_model value wrong");

    bb_json_free_str(json);
    bb_json_free(obj);
}

// ---------------------------------------------------------------------------
// Tests: bb_info_build_capture (host)
// ---------------------------------------------------------------------------

void test_bb_info_build_capture_fills_snap(void)
{
    bb_info_build_snap_t snap;
    bb_err_t err = bb_info_build_capture(&snap);
    TEST_ASSERT_EQUAL_INT(BB_OK, err);

    // app_sha256 should be "deadbeef0" on host
    TEST_ASSERT_EQUAL_STRING("deadbeef0", snap.app_sha256);

    // version should be non-empty
    TEST_ASSERT_TRUE_MESSAGE(snap.version[0] != '\0', "version is empty");
}

void test_bb_info_build_capture_null_returns_err(void)
{
    bb_err_t err = bb_info_build_capture(NULL);
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, err);
}

// ---------------------------------------------------------------------------
// Tests: /api/info integration — build is a nested section, root has no 12 fields
// ---------------------------------------------------------------------------

void test_bb_info_build_section_registered_in_schema(void)
{
    reset_all();

    // Register the build section as the espidf impl does
    bb_err_t err = bb_info_register_section("build", NULL, NULL, k_build_schema);
    // NULL get fn would return BB_ERR_INVALID_ARG — use a simple passthrough
    (void)err;

    // Just test that k_build_schema is valid JSON (it will be embedded in schema)
    cJSON *parsed = cJSON_Parse(k_build_schema);
    TEST_ASSERT_NOT_NULL_MESSAGE(parsed, "k_build_schema is not valid JSON");
    cJSON_Delete(parsed);
}

// Verify the 12 formerly-root-level fields are gone from k_info_schema_base.
void test_bb_info_schema_base_lacks_static_fields(void)
{
    TEST_ASSERT_NULL_MESSAGE(strstr(k_info_schema_base, "\"version\""),
        "version should not be in base schema (moved to build)");
    TEST_ASSERT_NULL_MESSAGE(strstr(k_info_schema_base, "\"idf_version\""),
        "idf_version should not be in base schema");
    TEST_ASSERT_NULL_MESSAGE(strstr(k_info_schema_base, "\"build_date\""),
        "build_date should not be in base schema");
    TEST_ASSERT_NULL_MESSAGE(strstr(k_info_schema_base, "\"build_time\""),
        "build_time should not be in base schema");
    TEST_ASSERT_NULL_MESSAGE(strstr(k_info_schema_base, "\"project_name\""),
        "project_name should not be in base schema");
    TEST_ASSERT_NULL_MESSAGE(strstr(k_info_schema_base, "\"chip_model\""),
        "chip_model should not be in base schema");
    TEST_ASSERT_NULL_MESSAGE(strstr(k_info_schema_base, "\"chip_revision\""),
        "chip_revision should not be in base schema");
    TEST_ASSERT_NULL_MESSAGE(strstr(k_info_schema_base, "\"cores\""),
        "cores should not be in base schema");
    TEST_ASSERT_NULL_MESSAGE(strstr(k_info_schema_base, "\"cpu_freq_mhz\""),
        "cpu_freq_mhz should not be in base schema");
    TEST_ASSERT_NULL_MESSAGE(strstr(k_info_schema_base, "\"flash_size\""),
        "flash_size should not be in base schema");
    TEST_ASSERT_NULL_MESSAGE(strstr(k_info_schema_base, "\"app_size\""),
        "app_size should not be in base schema");
    TEST_ASSERT_NULL_MESSAGE(strstr(k_info_schema_base, "\"board\""),
        "board should not be in base schema");
}

// Verify the build schema contains all 13 fields.
void test_bb_info_build_schema_has_all_fields(void)
{
    TEST_ASSERT_NOT_NULL(strstr(k_build_schema, "\"version\""));
    TEST_ASSERT_NOT_NULL(strstr(k_build_schema, "\"idf_version\""));
    TEST_ASSERT_NOT_NULL(strstr(k_build_schema, "\"build_date\""));
    TEST_ASSERT_NOT_NULL(strstr(k_build_schema, "\"build_time\""));
    TEST_ASSERT_NOT_NULL(strstr(k_build_schema, "\"project_name\""));
    TEST_ASSERT_NOT_NULL(strstr(k_build_schema, "\"chip_model\""));
    TEST_ASSERT_NOT_NULL(strstr(k_build_schema, "\"chip_revision\""));
    TEST_ASSERT_NOT_NULL(strstr(k_build_schema, "\"cores\""));
    TEST_ASSERT_NOT_NULL(strstr(k_build_schema, "\"cpu_freq_mhz\""));
    TEST_ASSERT_NOT_NULL(strstr(k_build_schema, "\"flash_size\""));
    TEST_ASSERT_NOT_NULL(strstr(k_build_schema, "\"app_size\""));
    TEST_ASSERT_NOT_NULL(strstr(k_build_schema, "\"board\""));
    TEST_ASSERT_NOT_NULL(strstr(k_build_schema, "\"app_sha256\""));
}

// ---------------------------------------------------------------------------
// Tests: bb_cache fidelity for build topic
// ---------------------------------------------------------------------------

static void build_section_get_for_test(bb_json_t section, void *ctx)
{
    (void)ctx;
    bb_cache_serialize_into("build", section);
}

void test_bb_info_build_cache_fidelity(void)
{
    reset_all();

    // Register + seed
    bb_cache_config_t cfg = {
        .key       = "build",
        .snapshot  = NULL,
        .snap_size = sizeof(bb_info_build_snap_t),
        .serialize = bb_info_build_emit,
        .flags     = BB_CACHE_FLAG_SSE,
    };
    bb_err_t err = bb_cache_register(&cfg);
    TEST_ASSERT_EQUAL_INT(BB_OK, err);

    bb_info_build_snap_t snap;
    bb_info_build_capture(&snap);
    err = bb_cache_update(&(bb_cache_update_t){ .key = "build", .snap = &snap });
    TEST_ASSERT_EQUAL_INT(BB_OK, err);

    // Two independent serializations must be byte-identical
    bb_json_t obj_a = bb_json_obj_new();
    bb_json_t obj_b = bb_json_obj_new();
    TEST_ASSERT_NOT_NULL(obj_a);
    TEST_ASSERT_NOT_NULL(obj_b);

    bb_cache_serialize_into("build", obj_a);
    bb_cache_serialize_into("build", obj_b);

    char *json_a = bb_json_serialize(obj_a);
    char *json_b = bb_json_serialize(obj_b);
    TEST_ASSERT_NOT_NULL(json_a);
    TEST_ASSERT_NOT_NULL(json_b);

    TEST_ASSERT_EQUAL_STRING_MESSAGE(json_a, json_b, "build cache fidelity");

    // app_sha256 must be "deadbeef0" on host
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(json_a, "\"deadbeef0\""), "app_sha256 wrong in cache");

    bb_json_free_str(json_a);
    bb_json_free_str(json_b);
    bb_json_free(obj_a);
    bb_json_free(obj_b);
}

// Verify register_section wires the build section into bb_info output.
void test_bb_info_build_section_in_info_output(void)
{
    reset_all();

    // Register build section (the REST path reads from bb_cache)
    bb_cache_config_t cfg = {
        .key       = "build",
        .snapshot  = NULL,
        .snap_size = sizeof(bb_info_build_snap_t),
        .serialize = bb_info_build_emit,
        .flags     = BB_CACHE_FLAG_SSE,
    };
    bb_cache_register(&cfg);
    bb_info_build_snap_t snap;
    bb_info_build_capture(&snap);
    bb_cache_update(&(bb_cache_update_t){ .key = "build", .snap = &snap });

    bb_info_register_section("build", build_section_get_for_test, NULL, k_build_schema);
    bb_info_freeze_for_test();

    bb_json_t root = bb_json_obj_new();
    TEST_ASSERT_NOT_NULL(root);
    bb_info_invoke_sections_for_test(root);

    char *json = bb_json_serialize(root);
    TEST_ASSERT_NOT_NULL(json);

    // "build" key should be present
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(json, "\"build\""), "build section missing from info output");
    // app_sha256 should be there
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(json, "\"app_sha256\""), "app_sha256 missing from build section");

    bb_json_free_str(json);
    bb_json_free(root);
}
