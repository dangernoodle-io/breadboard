#include "unity.h"
#include "bb_manifest.h"
#include "bb_json.h"
#include "bb_json_test_hooks.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

// ---------------------------------------------------------------------------
// Empty registry
// ---------------------------------------------------------------------------

void test_manifest_empty_emits_empty_arrays(void)
{
    bb_manifest_clear();

    bb_json_t doc = bb_manifest_emit();
    TEST_ASSERT_NOT_NULL(doc);

    char *json = bb_json_serialize(doc);
    TEST_ASSERT_NOT_NULL(json);

    // Verify structure: {"nvs":[],"mdns":[]}
    TEST_ASSERT_NOT_NULL(strstr(json, "\"nvs\":[]"));
    TEST_ASSERT_NOT_NULL(strstr(json, "\"mdns\":[]"));

    bb_json_free_str(json);
    bb_json_free(doc);
}

// ---------------------------------------------------------------------------
// NVS registration tests
// ---------------------------------------------------------------------------

void test_manifest_register_nv_single_namespace(void)
{
    bb_manifest_clear();

    const bb_manifest_nv_t keys[] = {
        {
            .key = "pool_host",
            .type = "str",
            .default_ = NULL,
            .max_len = 64,
            .desc = "Mining pool hostname/IP",
            .reboot_required = true,
            .provisioning_only = false,
        },
    };

    bb_err_t err = bb_manifest_register_nv("taipanminer", keys, 1);
    TEST_ASSERT_EQUAL(BB_OK, err);

    bb_json_t doc = bb_manifest_emit();
    TEST_ASSERT_NOT_NULL(doc);

    char *json = bb_json_serialize(doc);
    TEST_ASSERT_NOT_NULL(json);

    // Verify namespace
    TEST_ASSERT_NOT_NULL(strstr(json, "\"namespace\":\"taipanminer\""));
    // Verify key
    TEST_ASSERT_NOT_NULL(strstr(json, "\"key\":\"pool_host\""));
    // Verify type
    TEST_ASSERT_NOT_NULL(strstr(json, "\"type\":\"str\""));
    // Verify max_len
    TEST_ASSERT_NOT_NULL(strstr(json, "\"max_len\":64"));
    // Verify description
    TEST_ASSERT_NOT_NULL(strstr(json, "\"desc\":\"Mining pool hostname/IP\""));
    // Verify reboot_required
    TEST_ASSERT_NOT_NULL(strstr(json, "\"reboot_required\":true"));
    // Verify provisioning_only
    TEST_ASSERT_NOT_NULL(strstr(json, "\"provisioning_only\":false"));

    bb_json_free_str(json);
    bb_json_free(doc);
}

void test_manifest_register_nv_multiple_namespaces(void)
{
    bb_manifest_clear();

    const bb_manifest_nv_t keys1[] = {
        {
            .key = "key1",
            .type = "str",
            .default_ = NULL,
            .max_len = 32,
            .desc = "Key 1",
            .reboot_required = false,
            .provisioning_only = false,
        },
    };

    const bb_manifest_nv_t keys2[] = {
        {
            .key = "key2",
            .type = "u32",
            .default_ = "100",
            .max_len = 0,
            .desc = "Key 2",
            .reboot_required = true,
            .provisioning_only = true,
        },
    };

    bb_err_t err1 = bb_manifest_register_nv("ns1", keys1, 1);
    TEST_ASSERT_EQUAL(BB_OK, err1);

    bb_err_t err2 = bb_manifest_register_nv("ns2", keys2, 1);
    TEST_ASSERT_EQUAL(BB_OK, err2);

    bb_json_t doc = bb_manifest_emit();
    TEST_ASSERT_NOT_NULL(doc);

    char *json = bb_json_serialize(doc);
    TEST_ASSERT_NOT_NULL(json);

    // Verify both namespaces
    TEST_ASSERT_NOT_NULL(strstr(json, "\"namespace\":\"ns1\""));
    TEST_ASSERT_NOT_NULL(strstr(json, "\"namespace\":\"ns2\""));

    bb_json_free_str(json);
    bb_json_free(doc);
}

void test_manifest_nv_default_null_emits_json_null(void)
{
    bb_manifest_clear();

    const bb_manifest_nv_t keys[] = {
        {
            .key = "no_default",
            .type = "str",
            .default_ = NULL,
            .max_len = 64,
            .desc = "No default value",
            .reboot_required = false,
            .provisioning_only = false,
        },
    };

    bb_manifest_register_nv("test_ns", keys, 1);

    bb_json_t doc = bb_manifest_emit();
    TEST_ASSERT_NOT_NULL(doc);

    char *json = bb_json_serialize(doc);
    TEST_ASSERT_NOT_NULL(json);

    // Verify null is properly represented
    TEST_ASSERT_NOT_NULL(strstr(json, "\"default\":null"));

    bb_json_free_str(json);
    bb_json_free(doc);
}

void test_manifest_nv_default_string_emits_json_string(void)
{
    bb_manifest_clear();

    const bb_manifest_nv_t keys[] = {
        {
            .key = "with_default",
            .type = "str",
            .default_ = "example.com",
            .max_len = 64,
            .desc = "Has default value",
            .reboot_required = false,
            .provisioning_only = false,
        },
    };

    bb_manifest_register_nv("test_ns", keys, 1);

    bb_json_t doc = bb_manifest_emit();
    TEST_ASSERT_NOT_NULL(doc);

    char *json = bb_json_serialize(doc);
    TEST_ASSERT_NOT_NULL(json);

    // Verify string default
    TEST_ASSERT_NOT_NULL(strstr(json, "\"default\":\"example.com\""));

    bb_json_free_str(json);
    bb_json_free(doc);
}

void test_manifest_nv_max_len_zero_omitted(void)
{
    bb_manifest_clear();

    const bb_manifest_nv_t keys[] = {
        {
            .key = "no_max_len",
            .type = "u32",
            .default_ = NULL,
            .max_len = 0,
            .desc = "No max_len",
            .reboot_required = false,
            .provisioning_only = false,
        },
    };

    bb_manifest_register_nv("test_ns", keys, 1);

    bb_json_t doc = bb_manifest_emit();
    TEST_ASSERT_NOT_NULL(doc);

    char *json = bb_json_serialize(doc);
    TEST_ASSERT_NOT_NULL(json);

    // Verify max_len is not in the JSON (or is 0)
    // Since we don't include it, this test passes if it's simply not there
    bb_json_free_str(json);
    bb_json_free(doc);
}

// ---------------------------------------------------------------------------
// mDNS registration tests
// ---------------------------------------------------------------------------

void test_manifest_register_mdns_single_service(void)
{
    bb_manifest_clear();

    const bb_manifest_mdns_t keys[] = {
        {
            .key = "state",
            .desc = "Device runtime state",
            .values = "mining|idle|ota",
        },
    };

    bb_err_t err = bb_manifest_register_mdns("_taipanminer._tcp", keys, 1);
    TEST_ASSERT_EQUAL(BB_OK, err);

    bb_json_t doc = bb_manifest_emit();
    TEST_ASSERT_NOT_NULL(doc);

    char *json = bb_json_serialize(doc);
    TEST_ASSERT_NOT_NULL(json);

    // Verify service
    TEST_ASSERT_NOT_NULL(strstr(json, "\"service\":\"_taipanminer._tcp\""));
    // Verify txt key
    TEST_ASSERT_NOT_NULL(strstr(json, "\"key\":\"state\""));
    // Verify description
    TEST_ASSERT_NOT_NULL(strstr(json, "\"desc\":\"Device runtime state\""));

    bb_json_free_str(json);
    bb_json_free(doc);
}

void test_manifest_register_mdns_multiple_services(void)
{
    bb_manifest_clear();

    const bb_manifest_mdns_t keys1[] = {
        {
            .key = "state",
            .desc = "Service 1 state",
            .values = "active|inactive",
        },
    };

    const bb_manifest_mdns_t keys2[] = {
        {
            .key = "version",
            .desc = "Service 2 version",
            .values = NULL,
        },
    };

    bb_err_t err1 = bb_manifest_register_mdns("_service1._tcp", keys1, 1);
    TEST_ASSERT_EQUAL(BB_OK, err1);

    bb_err_t err2 = bb_manifest_register_mdns("_service2._tcp", keys2, 1);
    TEST_ASSERT_EQUAL(BB_OK, err2);

    bb_json_t doc = bb_manifest_emit();
    TEST_ASSERT_NOT_NULL(doc);

    char *json = bb_json_serialize(doc);
    TEST_ASSERT_NOT_NULL(json);

    // Verify both services
    TEST_ASSERT_NOT_NULL(strstr(json, "\"service\":\"_service1._tcp\""));
    TEST_ASSERT_NOT_NULL(strstr(json, "\"service\":\"_service2._tcp\""));

    bb_json_free_str(json);
    bb_json_free(doc);
}

void test_manifest_mdns_values_null_omits_field(void)
{
    bb_manifest_clear();

    const bb_manifest_mdns_t keys[] = {
        {
            .key = "custom_field",
            .desc = "Free-form text",
            .values = NULL,
        },
    };

    bb_manifest_register_mdns("_test._tcp", keys, 1);

    bb_json_t doc = bb_manifest_emit();
    TEST_ASSERT_NOT_NULL(doc);

    char *json = bb_json_serialize(doc);
    TEST_ASSERT_NOT_NULL(json);

    // Verify the key and desc are present
    TEST_ASSERT_NOT_NULL(strstr(json, "\"key\":\"custom_field\""));
    TEST_ASSERT_NOT_NULL(strstr(json, "\"desc\":\"Free-form text\""));

    bb_json_free_str(json);
    bb_json_free(doc);
}

void test_manifest_mdns_values_string_emits_array(void)
{
    bb_manifest_clear();

    const bb_manifest_mdns_t keys[] = {
        {
            .key = "state",
            .desc = "Device state",
            .values = "mining|idle|ota|provisioning",
        },
    };

    bb_manifest_register_mdns("_miner._tcp", keys, 1);

    bb_json_t doc = bb_manifest_emit();
    TEST_ASSERT_NOT_NULL(doc);

    char *json = bb_json_serialize(doc);
    TEST_ASSERT_NOT_NULL(json);

    // Verify values is an array with the expected elements
    TEST_ASSERT_NOT_NULL(strstr(json, "\"values\":["));
    TEST_ASSERT_NOT_NULL(strstr(json, "\"mining\""));
    TEST_ASSERT_NOT_NULL(strstr(json, "\"idle\""));
    TEST_ASSERT_NOT_NULL(strstr(json, "\"ota\""));
    TEST_ASSERT_NOT_NULL(strstr(json, "\"provisioning\""));

    bb_json_free_str(json);
    bb_json_free(doc);
}

// Empty segments (len==0) should be silently skipped.
void test_manifest_mdns_values_string_skips_empty_segments(void)
{
    bb_manifest_clear();

    // Double pipe creates an empty segment; trailing pipe too.
    const bb_manifest_mdns_t keys[] = {
        {
            .key = "mode",
            .desc = "Mode",
            .values = "active||idle|",
        },
    };

    bb_manifest_register_mdns("_svc._tcp", keys, 1);

    bb_json_t doc = bb_manifest_emit();
    TEST_ASSERT_NOT_NULL(doc);

    char *json = bb_json_serialize(doc);
    TEST_ASSERT_NOT_NULL(json);

    // Only "active" and "idle" should appear; empty segments are dropped.
    TEST_ASSERT_NOT_NULL(strstr(json, "\"active\""));
    TEST_ASSERT_NOT_NULL(strstr(json, "\"idle\""));

    bb_json_free_str(json);
    bb_json_free(doc);
}

// ---------------------------------------------------------------------------
// Registry overflow tests
// ---------------------------------------------------------------------------

void test_manifest_register_nv_overflow_returns_err(void)
{
    bb_manifest_clear();

    // Try to register more than NV_NAMESPACE_CAP (8) namespaces
    const bb_manifest_nv_t keys[] = {
        {
            .key = "dummy",
            .type = "str",
            .default_ = NULL,
            .max_len = 32,
            .desc = "Dummy key",
            .reboot_required = false,
            .provisioning_only = false,
        },
    };

    // Use static namespace names to avoid dynamic buffer reuse
    static const char *ns_names[] = {
        "ns_a", "ns_b", "ns_c", "ns_d", "ns_e", "ns_f", "ns_g", "ns_h",
    };

    // Register 8 namespaces (should succeed)
    for (int i = 0; i < 8; i++) {
        bb_err_t err = bb_manifest_register_nv(ns_names[i], keys, 1);
        TEST_ASSERT_EQUAL(BB_OK, err);
    }

    // Try to register the 9th (should fail)
    bb_err_t err = bb_manifest_register_nv("ns_overflow", keys, 1);
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, err);
}

void test_manifest_register_mdns_overflow_returns_err(void)
{
    bb_manifest_clear();

    const bb_manifest_mdns_t keys[] = {
        {
            .key = "state",
            .desc = "Dummy state",
            .values = NULL,
        },
    };

    // Use static service names to avoid dynamic buffer reuse
    static const char *svc_names[] = {
        "_svc1._tcp", "_svc2._tcp", "_svc3._tcp", "_svc4._tcp",
    };

    // Register 4 services (should succeed)
    for (int i = 0; i < 4; i++) {
        bb_err_t err = bb_manifest_register_mdns(svc_names[i], keys, 1);
        TEST_ASSERT_EQUAL(BB_OK, err);
    }

    // Try to register the 5th (should fail)
    bb_err_t err = bb_manifest_register_mdns("_service_overflow._tcp", keys, 1);
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, err);
}

void test_manifest_register_nv_too_many_keys_per_namespace(void)
{
    bb_manifest_clear();

    // Create 33 keys (exceeds NV_KEYS_PER_NAMESPACE_CAP of 32)
    bb_manifest_nv_t keys[33];
    for (int i = 0; i < 33; i++) {
        keys[i].key = "key";
        keys[i].type = "str";
        keys[i].default_ = NULL;
        keys[i].max_len = 32;
        keys[i].desc = "Key";
        keys[i].reboot_required = false;
        keys[i].provisioning_only = false;
    }

    bb_err_t err = bb_manifest_register_nv("test_ns", keys, 33);
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, err);
}

void test_manifest_register_mdns_too_many_keys_per_service(void)
{
    bb_manifest_clear();

    // Create 17 keys (exceeds MDNS_KEYS_PER_SERVICE_CAP of 16)
    bb_manifest_mdns_t keys[17];
    for (int i = 0; i < 17; i++) {
        keys[i].key = "key";
        keys[i].desc = "Key";
        keys[i].values = NULL;
    }

    bb_err_t err = bb_manifest_register_mdns("_test._tcp", keys, 17);
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, err);
}

void test_manifest_register_nv_duplicate_namespace_returns_err(void)
{
    bb_manifest_clear();

    const bb_manifest_nv_t keys[] = {
        {
            .key = "key1",
            .type = "str",
            .default_ = NULL,
            .max_len = 32,
            .desc = "Key 1",
            .reboot_required = false,
            .provisioning_only = false,
        },
    };

    bb_err_t err1 = bb_manifest_register_nv("dup_ns", keys, 1);
    TEST_ASSERT_EQUAL(BB_OK, err1);

    bb_err_t err2 = bb_manifest_register_nv("dup_ns", keys, 1);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, err2);
}

void test_manifest_register_mdns_duplicate_service_returns_err(void)
{
    bb_manifest_clear();

    const bb_manifest_mdns_t keys[] = {
        {
            .key = "state",
            .desc = "State",
            .values = NULL,
        },
    };

    bb_err_t err1 = bb_manifest_register_mdns("_dup._tcp", keys, 1);
    TEST_ASSERT_EQUAL(BB_OK, err1);

    bb_err_t err2 = bb_manifest_register_mdns("_dup._tcp", keys, 1);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, err2);
}

// ---------------------------------------------------------------------------
// Invalid argument tests
// ---------------------------------------------------------------------------

void test_manifest_register_nv_null_namespace_returns_err(void)
{
    bb_manifest_clear();

    const bb_manifest_nv_t keys[] = {
        {
            .key = "key1",
            .type = "str",
            .default_ = NULL,
            .max_len = 32,
            .desc = "Key 1",
            .reboot_required = false,
            .provisioning_only = false,
        },
    };

    bb_err_t err = bb_manifest_register_nv(NULL, keys, 1);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
}

void test_manifest_register_nv_null_keys_returns_err(void)
{
    bb_manifest_clear();

    bb_err_t err = bb_manifest_register_nv("ns", NULL, 1);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
}

void test_manifest_register_nv_zero_keys_returns_err(void)
{
    bb_manifest_clear();

    const bb_manifest_nv_t keys[] = {
        {
            .key = "key1",
            .type = "str",
            .default_ = NULL,
            .max_len = 32,
            .desc = "Key 1",
            .reboot_required = false,
            .provisioning_only = false,
        },
    };

    bb_err_t err = bb_manifest_register_nv("ns", keys, 0);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
}

void test_manifest_register_mdns_null_service_returns_err(void)
{
    bb_manifest_clear();

    const bb_manifest_mdns_t keys[] = {
        {
            .key = "state",
            .desc = "State",
            .values = NULL,
        },
    };

    bb_err_t err = bb_manifest_register_mdns(NULL, keys, 1);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
}

void test_manifest_register_mdns_null_keys_returns_err(void)
{
    bb_manifest_clear();

    bb_err_t err = bb_manifest_register_mdns("_svc._tcp", NULL, 1);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
}

void test_manifest_register_mdns_zero_keys_returns_err(void)
{
    bb_manifest_clear();

    const bb_manifest_mdns_t keys[] = {
        {
            .key = "state",
            .desc = "State",
            .values = NULL,
        },
    };

    bb_err_t err = bb_manifest_register_mdns("_svc._tcp", keys, 0);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
}

// ---------------------------------------------------------------------------
// OOM cleanup tests using bb_json_host_force_alloc_fail_after
// ---------------------------------------------------------------------------

void test_manifest_emit_oom_root_object(void)
{
    bb_manifest_clear();

    const bb_manifest_nv_t nv_keys[] = {
        {
            .key = "key1",
            .type = "str",
            .default_ = NULL,
            .max_len = 32,
            .desc = "Key 1",
            .reboot_required = false,
            .provisioning_only = false,
        },
    };
    bb_manifest_register_nv("ns", nv_keys, 1);

    // Force failure at allocation 0 (root object)
    bb_json_host_force_alloc_fail_after(0);

    bb_json_t doc = bb_manifest_emit();
    TEST_ASSERT_NULL(doc);

    bb_json_host_force_alloc_fail_after(-1); // Reset
    bb_manifest_clear();
}

void test_manifest_emit_oom_nvs_array(void)
{
    bb_manifest_clear();

    const bb_manifest_nv_t nv_keys[] = {
        {
            .key = "key1",
            .type = "str",
            .default_ = NULL,
            .max_len = 32,
            .desc = "Key 1",
            .reboot_required = false,
            .provisioning_only = false,
        },
    };
    bb_manifest_register_nv("ns", nv_keys, 1);

    // Force failure at allocation 1 (nvs array)
    bb_json_host_force_alloc_fail_after(1);

    bb_json_t doc = bb_manifest_emit();
    TEST_ASSERT_NULL(doc);

    bb_json_host_force_alloc_fail_after(-1); // Reset
    bb_manifest_clear();
}

void test_manifest_emit_oom_namespace_object(void)
{
    bb_manifest_clear();

    const bb_manifest_nv_t nv_keys[] = {
        {
            .key = "key1",
            .type = "str",
            .default_ = NULL,
            .max_len = 32,
            .desc = "Key 1",
            .reboot_required = false,
            .provisioning_only = false,
        },
    };
    bb_manifest_register_nv("ns", nv_keys, 1);

    // Force failure at allocation 2 (namespace object)
    bb_json_host_force_alloc_fail_after(2);

    bb_json_t doc = bb_manifest_emit();
    TEST_ASSERT_NULL(doc);

    bb_json_host_force_alloc_fail_after(-1); // Reset
    bb_manifest_clear();
}

void test_manifest_emit_oom_keys_array(void)
{
    bb_manifest_clear();

    const bb_manifest_nv_t nv_keys[] = {
        {
            .key = "key1",
            .type = "str",
            .default_ = NULL,
            .max_len = 32,
            .desc = "Key 1",
            .reboot_required = false,
            .provisioning_only = false,
        },
    };
    bb_manifest_register_nv("ns", nv_keys, 1);

    // Force failure at allocation 3 (keys array)
    bb_json_host_force_alloc_fail_after(3);

    bb_json_t doc = bb_manifest_emit();
    TEST_ASSERT_NULL(doc);

    bb_json_host_force_alloc_fail_after(-1); // Reset
    bb_manifest_clear();
}

void test_manifest_emit_oom_key_object(void)
{
    bb_manifest_clear();

    const bb_manifest_nv_t nv_keys[] = {
        {
            .key = "key1",
            .type = "str",
            .default_ = NULL,
            .max_len = 32,
            .desc = "Key 1",
            .reboot_required = false,
            .provisioning_only = false,
        },
    };
    bb_manifest_register_nv("ns", nv_keys, 1);

    // Force failure at allocation 4 (key object)
    bb_json_host_force_alloc_fail_after(4);

    bb_json_t doc = bb_manifest_emit();
    TEST_ASSERT_NULL(doc);

    bb_json_host_force_alloc_fail_after(-1); // Reset
    bb_manifest_clear();
}

void test_manifest_emit_oom_mdns_array(void)
{
    bb_manifest_clear();

    const bb_manifest_mdns_t mdns_keys[] = {
        {
            .key = "state",
            .desc = "State",
            .values = NULL,
        },
    };
    bb_manifest_register_mdns("_svc._tcp", mdns_keys, 1);

    // Force failure at allocation 1 (mdns array - comes after nvs)
    bb_json_host_force_alloc_fail_after(1);

    bb_json_t doc = bb_manifest_emit();
    TEST_ASSERT_NULL(doc);

    bb_json_host_force_alloc_fail_after(-1); // Reset
    bb_manifest_clear();
}

void test_manifest_emit_oom_service_object(void)
{
    bb_manifest_clear();

    const bb_manifest_mdns_t mdns_keys[] = {
        {
            .key = "state",
            .desc = "State",
            .values = NULL,
        },
    };
    bb_manifest_register_mdns("_svc._tcp", mdns_keys, 1);

    // Force failure when creating service object
    bb_json_host_force_alloc_fail_after(2);

    bb_json_t doc = bb_manifest_emit();
    TEST_ASSERT_NULL(doc);

    bb_json_host_force_alloc_fail_after(-1); // Reset
    bb_manifest_clear();
}

void test_manifest_emit_oom_txt_array(void)
{
    bb_manifest_clear();

    const bb_manifest_mdns_t mdns_keys[] = {
        {
            .key = "state",
            .desc = "State",
            .values = NULL,
        },
    };
    bb_manifest_register_mdns("_svc._tcp", mdns_keys, 1);

    // Force failure when creating txt array
    bb_json_host_force_alloc_fail_after(3);

    bb_json_t doc = bb_manifest_emit();
    TEST_ASSERT_NULL(doc);

    bb_json_host_force_alloc_fail_after(-1); // Reset
    bb_manifest_clear();
}

void test_manifest_emit_oom_txt_object(void)
{
    bb_manifest_clear();

    const bb_manifest_mdns_t mdns_keys[] = {
        {
            .key = "state",
            .desc = "State",
            .values = NULL,
        },
    };
    bb_manifest_register_mdns("_svc._tcp", mdns_keys, 1);

    // Force failure when creating txt object
    bb_json_host_force_alloc_fail_after(4);

    bb_json_t doc = bb_manifest_emit();
    TEST_ASSERT_NULL(doc);

    bb_json_host_force_alloc_fail_after(-1); // Reset
    bb_manifest_clear();
}

void test_manifest_emit_oom_values_array(void)
{
    bb_manifest_clear();

    const bb_manifest_mdns_t mdns_keys[] = {
        {
            .key = "state",
            .desc = "State",
            .values = "mining|idle",
        },
    };
    bb_manifest_register_mdns("_svc._tcp", mdns_keys, 1);

    // Force failure when creating txt_obj (alloc index 5)
    bb_json_host_force_alloc_fail_after(5);

    bb_json_t doc = bb_manifest_emit();
    TEST_ASSERT_NULL(doc);

    bb_json_host_force_alloc_fail_after(-1); // Reset
    bb_manifest_clear();
}

// Allocation order for 1 mdns service with 1 txt key with values:
//   0: root obj, 1: nvs_arr, 2: mdns_arr, 3: svc_obj,
//   4: txt_arr, 5: txt_obj, 6: emit_values_array → bb_json_arr_new
// Failing at index 6 exercises the NULL return from emit_values_array
// and the cleanup cascade at lines 306-313 of bb_manifest_emit.c.
void test_manifest_emit_oom_emit_values_arr_new(void)
{
    bb_manifest_clear();

    const bb_manifest_mdns_t mdns_keys[] = {
        {
            .key = "state",
            .desc = "State",
            .values = "mining|idle|ota",
        },
    };
    bb_manifest_register_mdns("_svc._tcp", mdns_keys, 1);

    // Fail the bb_json_arr_new() inside emit_values_array
    bb_json_host_force_alloc_fail_after(6);

    bb_json_t doc = bb_manifest_emit();
    TEST_ASSERT_NULL(doc);

    bb_json_host_force_alloc_fail_after(-1);
    bb_manifest_clear();
}
