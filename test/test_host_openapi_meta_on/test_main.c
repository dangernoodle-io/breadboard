// test_main.c -- manual-registration consolidation for this test_filter
// dir (mirrors test/test_host/test_main.c's convention). PlatformIO links
// every .c file under a single test_filter dir into ONE binary, so exactly
// one main() may exist here: the pilot's own file
// (test_bb_diag_storage_nvs_route_wiring.c, B1-1059 PR-2) originally
// carried its own main() when it was this dir's only test file; B1-1059
// PR-b adds two more (mqtt/temp health schema wiring) and consolidates all
// three suites' RUN_TESTs here.
#include "unity.h"
#include "bb_serialize_meta_test.h"

// From test_bb_diag_storage_nvs_route_wiring.c
void test_bb_diag_storage_nvs_describe_schema_starts_null(void);
void test_bb_diag_storage_nvs_assemble_schema_patches_matching_content(void);
void test_bb_diag_storage_nvs_assemble_schema_idempotent_pointer_stable(void);

// From test_bb_diag_storage_partitions_route_wiring.c (B1-1059 PR-3 batch 1)
void test_bb_diag_storage_partitions_describe_schema_starts_null(void);
void test_bb_diag_storage_partitions_assemble_schema_offline_on_compose_failure(void);
void test_bb_diag_storage_partitions_assemble_schema_patches_matching_content(void);
void test_bb_diag_storage_partitions_assemble_schema_idempotent_pointer_stable(void);

// From test_bb_diag_meminfo_route_wiring.c (B1-1059 PR-3 batch 1)
void test_bb_diag_meminfo_describe_schema_starts_null(void);
void test_bb_diag_meminfo_assemble_schema_offline_on_compose_failure(void);
void test_bb_diag_meminfo_assemble_schema_patches_matching_content(void);
void test_bb_diag_meminfo_assemble_schema_idempotent_pointer_stable(void);

// From test_bb_ring_diag_route_wiring.c (B1-1059 PR-3 batch 1)
void test_bb_ring_diag_describe_schema_starts_null(void);
void test_bb_ring_diag_assemble_schema_offline_on_compose_failure(void);
void test_bb_ring_diag_assemble_schema_patches_matching_content(void);
void test_bb_ring_diag_assemble_schema_idempotent_pointer_stable(void);

// From test_bb_ws_server_diag_route_wiring.c (B1-1059 PR-3 batch 1)
void test_bb_ws_server_diag_describe_schema_starts_null(void);
void test_bb_ws_server_diag_assemble_schema_offline_on_compose_failure(void);
void test_bb_ws_server_diag_assemble_schema_patches_matching_content(void);
void test_bb_ws_server_diag_assemble_schema_idempotent_pointer_stable(void);

// From test_bb_serialize_meta_openapi_test_seam.c
void test_bb_serialize_meta_openapi_schema_force_no_space(void);
void test_bb_serialize_meta_openapi_fragment_force_no_space(void);
void test_bb_serialize_meta_openapi_schema_force_no_space_null_out_len(void);
void test_bb_serialize_meta_openapi_fragment_force_no_space_null_out_len(void);
void test_bb_serialize_meta_openapi_schema_overflow_too_small_cap(void);
void test_bb_serialize_meta_openapi_schema_overflow_null_out_len(void);
void test_bb_serialize_meta_openapi_schema_success_non_null_out_len(void);
void test_bb_serialize_meta_openapi_schema_success_null_out_len(void);
void test_bb_serialize_meta_openapi_fragment_overflow_too_small_cap(void);
void test_bb_serialize_meta_openapi_fragment_overflow_null_out_len(void);
void test_bb_serialize_meta_openapi_fragment_success_null_out_len(void);

// From test_bb_mqtt_client_health_schema_wiring.c
void test_bb_mqtt_client_health_register_offline_on_compose_failure(void);
void test_bb_mqtt_client_health_register_composes_matching_schema(void);
void test_bb_mqtt_client_health_register_idempotent_same_content(void);

// From test_bb_temp_health_schema_wiring.c
void test_bb_temp_register_info_offline_on_compose_failure(void);
void test_bb_temp_register_info_composes_matching_schema(void);
void test_bb_temp_register_info_idempotent_same_content(void);

void setUp(void) {}

// Defensive belt-and-suspenders for the BB_SERIALIZE_META_TESTING
// fail-injection seam (bb_serialize_meta_test.h): each
// *_offline_on_compose_failure test already resets it itself before
// returning, but tearDown() guarantees it can never leak into a later test
// even if a test fails/aborts partway through.
void tearDown(void)
{
    bb_serialize_meta_openapi_test_set_force_no_space(false);
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_bb_diag_storage_nvs_describe_schema_starts_null);
    RUN_TEST(test_bb_diag_storage_nvs_assemble_schema_patches_matching_content);
    RUN_TEST(test_bb_diag_storage_nvs_assemble_schema_idempotent_pointer_stable);

    // Fail-fast (offline-on-compose-failure) tests MUST run before their
    // component's success tests -- see each test's own comment (the
    // compose-and-patch step is guarded/idempotent, so a prior successful
    // compose would make the fail-injection seam a no-op).
    RUN_TEST(test_bb_diag_storage_partitions_describe_schema_starts_null);
    RUN_TEST(test_bb_diag_storage_partitions_assemble_schema_offline_on_compose_failure);
    RUN_TEST(test_bb_diag_storage_partitions_assemble_schema_patches_matching_content);
    RUN_TEST(test_bb_diag_storage_partitions_assemble_schema_idempotent_pointer_stable);

    RUN_TEST(test_bb_diag_meminfo_describe_schema_starts_null);
    RUN_TEST(test_bb_diag_meminfo_assemble_schema_offline_on_compose_failure);
    RUN_TEST(test_bb_diag_meminfo_assemble_schema_patches_matching_content);
    RUN_TEST(test_bb_diag_meminfo_assemble_schema_idempotent_pointer_stable);

    RUN_TEST(test_bb_ring_diag_describe_schema_starts_null);
    RUN_TEST(test_bb_ring_diag_assemble_schema_offline_on_compose_failure);
    RUN_TEST(test_bb_ring_diag_assemble_schema_patches_matching_content);
    RUN_TEST(test_bb_ring_diag_assemble_schema_idempotent_pointer_stable);

    RUN_TEST(test_bb_ws_server_diag_describe_schema_starts_null);
    RUN_TEST(test_bb_ws_server_diag_assemble_schema_offline_on_compose_failure);
    RUN_TEST(test_bb_ws_server_diag_assemble_schema_patches_matching_content);
    RUN_TEST(test_bb_ws_server_diag_assemble_schema_idempotent_pointer_stable);

    RUN_TEST(test_bb_serialize_meta_openapi_schema_force_no_space);
    RUN_TEST(test_bb_serialize_meta_openapi_fragment_force_no_space);
    RUN_TEST(test_bb_serialize_meta_openapi_schema_force_no_space_null_out_len);
    RUN_TEST(test_bb_serialize_meta_openapi_fragment_force_no_space_null_out_len);
    RUN_TEST(test_bb_serialize_meta_openapi_schema_overflow_too_small_cap);
    RUN_TEST(test_bb_serialize_meta_openapi_schema_overflow_null_out_len);
    RUN_TEST(test_bb_serialize_meta_openapi_schema_success_non_null_out_len);
    RUN_TEST(test_bb_serialize_meta_openapi_schema_success_null_out_len);
    RUN_TEST(test_bb_serialize_meta_openapi_fragment_overflow_too_small_cap);
    RUN_TEST(test_bb_serialize_meta_openapi_fragment_overflow_null_out_len);
    RUN_TEST(test_bb_serialize_meta_openapi_fragment_success_null_out_len);

    // Fail-fast (offline-on-compose-failure) tests MUST run before their
    // component's success tests -- see each test's own comment (the
    // compose-and-patch step is guarded/idempotent, so a prior successful
    // compose would make the fail-injection seam a no-op).
    RUN_TEST(test_bb_mqtt_client_health_register_offline_on_compose_failure);
    RUN_TEST(test_bb_mqtt_client_health_register_composes_matching_schema);
    RUN_TEST(test_bb_mqtt_client_health_register_idempotent_same_content);

    RUN_TEST(test_bb_temp_register_info_offline_on_compose_failure);
    RUN_TEST(test_bb_temp_register_info_composes_matching_schema);
    RUN_TEST(test_bb_temp_register_info_idempotent_same_content);

    return UNITY_END();
}
