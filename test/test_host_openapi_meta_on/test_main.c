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

// From test_bb_serialize_meta_ensure_composed.c (B1-1204)
void test_bb_serialize_meta_ensure_composed_composes_when_buf_empty(void);
void test_bb_serialize_meta_ensure_composed_overflow_leaves_buf_empty(void);
void test_bb_serialize_meta_ensure_composed_idempotent_second_call_is_noop(void);
void test_bb_serialize_meta_ensure_composed_dispatches_schema_composer(void);
void test_bb_serialize_meta_ensure_composed_dispatches_fragment_composer(void);

// From test_bb_serialize_meta_ensure_topic_schema.c (B1-1059 SSE batch PR-1)
void test_bb_serialize_meta_openapi_topic_schema_composes_exact_prefix_and_body(void);
void test_bb_serialize_meta_openapi_topic_schema_force_no_space(void);
void test_bb_serialize_meta_openapi_topic_schema_force_no_space_null_out_len(void);
void test_bb_serialize_meta_openapi_topic_schema_prefix_overflow_leaves_buf_empty(void);
void test_bb_serialize_meta_openapi_topic_schema_prefix_overflow_null_out_len(void);
void test_bb_serialize_meta_openapi_topic_schema_composer_overflow_leaves_buf_empty(void);
void test_bb_serialize_meta_openapi_topic_schema_composer_overflow_null_out_len(void);
void test_bb_serialize_meta_openapi_topic_schema_success_null_out_len(void);
void test_bb_serialize_meta_openapi_topic_schema_rejects_zero_len_composer_body(void);
void test_bb_serialize_meta_openapi_topic_schema_rejects_non_brace_composer_body_null_out_len(void);
void test_bb_serialize_meta_openapi_topic_schema_escapes_title_and_topic(void);
void test_bb_serialize_meta_ensure_topic_schema_composes_when_buf_empty(void);
void test_bb_serialize_meta_ensure_topic_schema_overflow_leaves_buf_empty(void);
void test_bb_serialize_meta_ensure_topic_schema_idempotent_second_call_is_noop(void);
void test_bb_serialize_meta_ensure_topic_schema_dispatches_schema_composer(void);

// From test_bb_ota_check_update_available_wiring.c (B1-1059 SSE batch PR-2)
void test_bb_ota_check_update_available_schema_offline_on_compose_failure(void);
void test_bb_ota_check_update_available_schema_matches_expected_content(void);
void test_bb_ota_check_update_available_schema_idempotent_pointer_stable(void);

// From test_bb_ota_check_config_get_wiring.c (B1-1059 emit batch A, site 3)
void test_bb_ota_check_config_get_schema_offline_on_compose_failure(void);
void test_bb_ota_check_config_get_schema_matches_expected_content(void);
void test_bb_ota_check_config_get_schema_idempotent_pointer_stable(void);

// From test_bb_ota_check_config_post_wiring.c (B1-1059 emit batch A, site 4)
void test_bb_ota_check_config_post_request_schema_offline_on_compose_failure(void);
void test_bb_ota_check_config_post_response_schema_offline_on_compose_failure(void);
void test_bb_ota_check_config_post_request_schema_matches_expected_content(void);
void test_bb_ota_check_config_post_response_schema_matches_expected_content(void);
void test_bb_ota_check_config_post_request_schema_idempotent_pointer_stable(void);
void test_bb_ota_check_config_post_response_schema_idempotent_pointer_stable(void);

// From test_bb_storage_http_delete_route_wiring.c (B1-1059 emit batch A, site 1)
void test_bb_storage_http_delete_assemble_request_schema_offline_on_compose_failure(void);
void test_bb_storage_http_delete_assemble_request_schema_patches_matching_content(void);
void test_bb_storage_http_delete_assemble_request_schema_idempotent_pointer_stable(void);

// From test_bb_storage_http_factory_reset_route_wiring.c (B1-1059 emit batch A, site 5)
void test_bb_storage_http_factory_reset_assemble_request_schema_offline_on_compose_failure(void);
void test_bb_storage_http_factory_reset_assemble_request_schema_patches_matching_content(void);
void test_bb_storage_http_factory_reset_assemble_request_schema_idempotent_pointer_stable(void);

// From test_bb_diag_storage_nvs_route_wiring.c
void test_bb_diag_storage_nvs_assemble_schema_offline_on_compose_failure(void);
void test_bb_diag_storage_nvs_assemble_schema_patches_matching_content(void);
void test_bb_diag_storage_nvs_assemble_schema_idempotent_pointer_stable(void);

// From test_bb_diag_storage_partitions_route_wiring.c (B1-1059 PR-3 batch 1)
void test_bb_diag_storage_partitions_assemble_schema_offline_on_compose_failure(void);
void test_bb_diag_storage_partitions_assemble_schema_patches_matching_content(void);
void test_bb_diag_storage_partitions_assemble_schema_idempotent_pointer_stable(void);

// From test_bb_diag_meminfo_route_wiring.c (B1-1059 PR-3 batch 1)
void test_bb_diag_meminfo_assemble_schema_offline_on_compose_failure(void);
void test_bb_diag_meminfo_assemble_schema_patches_matching_content(void);
void test_bb_diag_meminfo_assemble_schema_idempotent_pointer_stable(void);

// From test_bb_ring_diag_route_wiring.c (B1-1059 PR-3 batch 1)
void test_bb_ring_diag_assemble_schema_offline_on_compose_failure(void);
void test_bb_ring_diag_assemble_schema_patches_matching_content(void);
void test_bb_ring_diag_assemble_schema_idempotent_pointer_stable(void);

// From test_bb_ws_server_diag_route_wiring.c (B1-1059 PR-3 batch 1)
void test_bb_ws_server_diag_assemble_schema_offline_on_compose_failure(void);
void test_bb_ws_server_diag_assemble_schema_patches_matching_content(void);
void test_bb_ws_server_diag_assemble_schema_idempotent_pointer_stable(void);

// From test_bb_wifi_http_diag_route_wiring.c (B1-1059 PR-3 batch 2)
void test_bb_wifi_http_diag_assemble_schema_offline_on_compose_failure(void);
void test_bb_wifi_http_diag_assemble_schema_patches_matching_content(void);
void test_bb_wifi_http_diag_assemble_schema_idempotent_pointer_stable(void);

// From test_bb_sensor_http_wire_route_wiring.c (B1-1059 PR-3 batch 3)
void test_bb_sensor_http_wire_assemble_fan_schema_offline_on_compose_failure(void);
void test_bb_sensor_http_wire_assemble_fan_schema_patches_matching_content(void);
void test_bb_sensor_http_wire_assemble_fan_schema_idempotent_pointer_stable(void);
void test_bb_sensor_http_wire_assemble_fan_request_schema_offline_on_compose_failure(void);
void test_bb_sensor_http_wire_assemble_fan_request_schema_patches_matching_content(void);
void test_bb_sensor_http_wire_assemble_fan_request_schema_idempotent_pointer_stable(void);
void test_bb_sensor_http_wire_assemble_power_schema_offline_on_compose_failure(void);
void test_bb_sensor_http_wire_assemble_power_schema_patches_matching_content(void);
void test_bb_sensor_http_wire_assemble_power_schema_idempotent_pointer_stable(void);
void test_bb_sensor_http_wire_assemble_thermal_schema_offline_on_compose_failure(void);
void test_bb_sensor_http_wire_assemble_thermal_schema_patches_matching_content(void);
void test_bb_sensor_http_wire_assemble_thermal_schema_idempotent_pointer_stable(void);

// From test_bb_system_reboot_route_wiring.c (B1-1059 emit batch A, site 2)
void test_bb_system_reboot_assemble_request_schema_offline_on_compose_failure(void);
void test_bb_system_reboot_assemble_request_schema_patches_matching_content(void);
void test_bb_system_reboot_assemble_request_schema_idempotent_pointer_stable(void);

// From test_bb_diag_boot_topic_wiring.c (B1-1059 SSE batch PR-3)
void test_bb_diag_boot_topic_schema_offline_on_compose_failure(void);
void test_bb_diag_boot_topic_schema_matches_expected_content(void);
void test_bb_diag_boot_topic_schema_idempotent_pointer_stable(void);

// From test_bb_health_stack_topic_wiring.c (B1-1059 SSE batch PR-3)
void test_bb_health_stack_topic_schema_offline_on_compose_failure(void);
void test_bb_health_stack_topic_schema_matches_expected_content(void);
void test_bb_health_stack_topic_schema_idempotent_pointer_stable(void);

// From test_bb_log_event_line_topic_wiring.c (B1-1059 SSE batch PR-3)
void test_bb_log_event_line_topic_schema_offline_on_compose_failure(void);
void test_bb_log_event_line_topic_schema_matches_expected_content(void);
void test_bb_log_event_line_topic_schema_idempotent_pointer_stable(void);

// From test_bb_ota_hooks_topic_wiring.c (B1-1059 SSE batch PR-3)
void test_bb_ota_hooks_topic_schema_offline_on_compose_failure(void);
void test_bb_ota_hooks_topic_schema_matches_expected_content(void);
void test_bb_ota_hooks_topic_schema_idempotent_pointer_stable(void);

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
void test_bb_serialize_meta_openapi_open_fragment_force_no_space(void);
void test_bb_serialize_meta_openapi_open_fragment_force_no_space_null_out_len(void);
void test_bb_serialize_meta_openapi_root_close_force_no_space(void);
void test_bb_serialize_meta_openapi_root_close_force_no_space_null_out_len(void);
void test_bb_serialize_meta_openapi_open_fragment_overflow_too_small_cap(void);
void test_bb_serialize_meta_openapi_open_fragment_overflow_null_out_len(void);
void test_bb_serialize_meta_openapi_open_fragment_success_null_out_len(void);
void test_bb_serialize_meta_openapi_root_close_overflow_too_small_cap(void);
void test_bb_serialize_meta_openapi_root_close_overflow_null_out_len(void);
void test_bb_serialize_meta_openapi_root_close_success_null_out_len(void);

// From test_bb_mqtt_client_health_schema_wiring.c
void test_bb_mqtt_client_health_register_offline_on_compose_failure(void);
void test_bb_mqtt_client_health_register_composes_matching_schema(void);
void test_bb_mqtt_client_health_register_idempotent_same_content(void);

// From test_bb_temp_health_schema_wiring.c
void test_bb_temp_register_info_offline_on_compose_failure(void);
void test_bb_temp_register_info_composes_matching_schema(void);
void test_bb_temp_register_info_idempotent_same_content(void);

// From test_bb_health_root_schema_wiring.c (B1-1059 PR-d)
void test_bb_health_assemble_schema_root_offline_on_compose_failure(void);
void test_bb_health_assemble_schema_root_composes_matching_schema(void);
void test_bb_health_assemble_schema_root_idempotent_same_content(void);
void test_bb_health_assemble_schema_root_close_failure_returns_null(void);
void test_bb_health_assemble_schema_root_malloc_failure_returns_null(void);
void test_bb_health_assemble_schema_root_skips_section_with_null_schema_props(void);
void test_bb_health_assemble_schema_root_close_cap_override_no_shrink_when_larger(void);

// From test_bb_display_info_topic_wiring.c (B1-1059 SSE PR-4)
void test_bb_display_info_topic_schema_offline_on_compose_failure(void);
void test_bb_display_info_topic_schema_matches_expected_content(void);
void test_bb_display_info_topic_schema_idempotent_pointer_stable(void);

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

    // Fail-fast (offline-on-compose-failure) MUST run before the success
    // tests below -- see each test's own comment. update_available,
    // config_get, and config_post's request/response all share ONE init()
    // call site (bb_ota_check_init(), B1-1059 emit batch A sites 3+4), in
    // that fixed compose order -- every site's own offline test primes
    // every EARLIER site's buffer directly (bypassing init()), so ALL FOUR
    // offline tests MUST run before ANY site's success test (a full
    // successful init() call composes every buffer as a side effect,
    // permanently defeating a not-yet-run offline test's fail-injection).
    // See test_bb_ota_check_config_get_wiring.c's / test_bb_ota_check_
    // config_post_wiring.c's own comments.
    RUN_TEST(test_bb_ota_check_update_available_schema_offline_on_compose_failure);
    RUN_TEST(test_bb_ota_check_config_get_schema_offline_on_compose_failure);
    RUN_TEST(test_bb_ota_check_config_post_request_schema_offline_on_compose_failure);
    RUN_TEST(test_bb_ota_check_config_post_response_schema_offline_on_compose_failure);

    RUN_TEST(test_bb_ota_check_update_available_schema_matches_expected_content);
    RUN_TEST(test_bb_ota_check_update_available_schema_idempotent_pointer_stable);

    RUN_TEST(test_bb_ota_check_config_get_schema_matches_expected_content);
    RUN_TEST(test_bb_ota_check_config_get_schema_idempotent_pointer_stable);

    RUN_TEST(test_bb_ota_check_config_post_request_schema_matches_expected_content);
    RUN_TEST(test_bb_ota_check_config_post_response_schema_matches_expected_content);
    RUN_TEST(test_bb_ota_check_config_post_request_schema_idempotent_pointer_stable);
    RUN_TEST(test_bb_ota_check_config_post_response_schema_idempotent_pointer_stable);

    RUN_TEST(test_bb_serialize_meta_ensure_composed_composes_when_buf_empty);
    RUN_TEST(test_bb_serialize_meta_ensure_composed_overflow_leaves_buf_empty);
    RUN_TEST(test_bb_serialize_meta_ensure_composed_idempotent_second_call_is_noop);
    RUN_TEST(test_bb_serialize_meta_ensure_composed_dispatches_schema_composer);
    RUN_TEST(test_bb_serialize_meta_ensure_composed_dispatches_fragment_composer);

    RUN_TEST(test_bb_serialize_meta_openapi_topic_schema_composes_exact_prefix_and_body);
    RUN_TEST(test_bb_serialize_meta_openapi_topic_schema_force_no_space);
    RUN_TEST(test_bb_serialize_meta_openapi_topic_schema_force_no_space_null_out_len);
    RUN_TEST(test_bb_serialize_meta_openapi_topic_schema_prefix_overflow_leaves_buf_empty);
    RUN_TEST(test_bb_serialize_meta_openapi_topic_schema_prefix_overflow_null_out_len);
    RUN_TEST(test_bb_serialize_meta_openapi_topic_schema_composer_overflow_leaves_buf_empty);
    RUN_TEST(test_bb_serialize_meta_openapi_topic_schema_composer_overflow_null_out_len);
    RUN_TEST(test_bb_serialize_meta_openapi_topic_schema_success_null_out_len);
    RUN_TEST(test_bb_serialize_meta_openapi_topic_schema_rejects_zero_len_composer_body);
    RUN_TEST(test_bb_serialize_meta_openapi_topic_schema_rejects_non_brace_composer_body_null_out_len);
    RUN_TEST(test_bb_serialize_meta_openapi_topic_schema_escapes_title_and_topic);
    RUN_TEST(test_bb_serialize_meta_ensure_topic_schema_composes_when_buf_empty);
    RUN_TEST(test_bb_serialize_meta_ensure_topic_schema_overflow_leaves_buf_empty);
    RUN_TEST(test_bb_serialize_meta_ensure_topic_schema_idempotent_second_call_is_noop);
    RUN_TEST(test_bb_serialize_meta_ensure_topic_schema_dispatches_schema_composer);

    RUN_TEST(test_bb_storage_http_delete_assemble_request_schema_offline_on_compose_failure);
    RUN_TEST(test_bb_storage_http_delete_assemble_request_schema_patches_matching_content);
    RUN_TEST(test_bb_storage_http_delete_assemble_request_schema_idempotent_pointer_stable);

    RUN_TEST(test_bb_storage_http_factory_reset_assemble_request_schema_offline_on_compose_failure);
    RUN_TEST(test_bb_storage_http_factory_reset_assemble_request_schema_patches_matching_content);
    RUN_TEST(test_bb_storage_http_factory_reset_assemble_request_schema_idempotent_pointer_stable);

    RUN_TEST(test_bb_diag_storage_nvs_assemble_schema_offline_on_compose_failure);
    RUN_TEST(test_bb_diag_storage_nvs_assemble_schema_patches_matching_content);
    RUN_TEST(test_bb_diag_storage_nvs_assemble_schema_idempotent_pointer_stable);

    // Fail-fast (offline-on-compose-failure) tests MUST run before their
    // component's success tests -- see each test's own comment (the
    // compose-and-patch step is guarded/idempotent, so a prior successful
    // compose would make the fail-injection seam a no-op).
    RUN_TEST(test_bb_diag_storage_partitions_assemble_schema_offline_on_compose_failure);
    RUN_TEST(test_bb_diag_storage_partitions_assemble_schema_patches_matching_content);
    RUN_TEST(test_bb_diag_storage_partitions_assemble_schema_idempotent_pointer_stable);

    RUN_TEST(test_bb_diag_meminfo_assemble_schema_offline_on_compose_failure);
    RUN_TEST(test_bb_diag_meminfo_assemble_schema_patches_matching_content);
    RUN_TEST(test_bb_diag_meminfo_assemble_schema_idempotent_pointer_stable);

    RUN_TEST(test_bb_ring_diag_assemble_schema_offline_on_compose_failure);
    RUN_TEST(test_bb_ring_diag_assemble_schema_patches_matching_content);
    RUN_TEST(test_bb_ring_diag_assemble_schema_idempotent_pointer_stable);

    RUN_TEST(test_bb_ws_server_diag_assemble_schema_offline_on_compose_failure);
    RUN_TEST(test_bb_ws_server_diag_assemble_schema_patches_matching_content);
    RUN_TEST(test_bb_ws_server_diag_assemble_schema_idempotent_pointer_stable);

    RUN_TEST(test_bb_wifi_http_diag_assemble_schema_offline_on_compose_failure);
    RUN_TEST(test_bb_wifi_http_diag_assemble_schema_patches_matching_content);
    RUN_TEST(test_bb_wifi_http_diag_assemble_schema_idempotent_pointer_stable);

    RUN_TEST(test_bb_sensor_http_wire_assemble_fan_schema_offline_on_compose_failure);
    RUN_TEST(test_bb_sensor_http_wire_assemble_fan_schema_patches_matching_content);
    RUN_TEST(test_bb_sensor_http_wire_assemble_fan_schema_idempotent_pointer_stable);

    RUN_TEST(test_bb_sensor_http_wire_assemble_fan_request_schema_offline_on_compose_failure);
    RUN_TEST(test_bb_sensor_http_wire_assemble_fan_request_schema_patches_matching_content);
    RUN_TEST(test_bb_sensor_http_wire_assemble_fan_request_schema_idempotent_pointer_stable);

    RUN_TEST(test_bb_sensor_http_wire_assemble_power_schema_offline_on_compose_failure);
    RUN_TEST(test_bb_sensor_http_wire_assemble_power_schema_patches_matching_content);
    RUN_TEST(test_bb_sensor_http_wire_assemble_power_schema_idempotent_pointer_stable);

    RUN_TEST(test_bb_sensor_http_wire_assemble_thermal_schema_offline_on_compose_failure);
    RUN_TEST(test_bb_sensor_http_wire_assemble_thermal_schema_patches_matching_content);
    RUN_TEST(test_bb_sensor_http_wire_assemble_thermal_schema_idempotent_pointer_stable);

    RUN_TEST(test_bb_system_reboot_assemble_request_schema_offline_on_compose_failure);
    RUN_TEST(test_bb_system_reboot_assemble_request_schema_patches_matching_content);
    RUN_TEST(test_bb_system_reboot_assemble_request_schema_idempotent_pointer_stable);

    RUN_TEST(test_bb_diag_boot_topic_schema_offline_on_compose_failure);
    RUN_TEST(test_bb_diag_boot_topic_schema_matches_expected_content);
    RUN_TEST(test_bb_diag_boot_topic_schema_idempotent_pointer_stable);

    RUN_TEST(test_bb_health_stack_topic_schema_offline_on_compose_failure);
    RUN_TEST(test_bb_health_stack_topic_schema_matches_expected_content);
    RUN_TEST(test_bb_health_stack_topic_schema_idempotent_pointer_stable);

    RUN_TEST(test_bb_log_event_line_topic_schema_offline_on_compose_failure);
    RUN_TEST(test_bb_log_event_line_topic_schema_matches_expected_content);
    RUN_TEST(test_bb_log_event_line_topic_schema_idempotent_pointer_stable);

    RUN_TEST(test_bb_ota_hooks_topic_schema_offline_on_compose_failure);
    RUN_TEST(test_bb_ota_hooks_topic_schema_matches_expected_content);
    RUN_TEST(test_bb_ota_hooks_topic_schema_idempotent_pointer_stable);

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
    RUN_TEST(test_bb_serialize_meta_openapi_open_fragment_force_no_space);
    RUN_TEST(test_bb_serialize_meta_openapi_open_fragment_force_no_space_null_out_len);
    RUN_TEST(test_bb_serialize_meta_openapi_root_close_force_no_space);
    RUN_TEST(test_bb_serialize_meta_openapi_root_close_force_no_space_null_out_len);
    RUN_TEST(test_bb_serialize_meta_openapi_open_fragment_overflow_too_small_cap);
    RUN_TEST(test_bb_serialize_meta_openapi_open_fragment_overflow_null_out_len);
    RUN_TEST(test_bb_serialize_meta_openapi_open_fragment_success_null_out_len);
    RUN_TEST(test_bb_serialize_meta_openapi_root_close_overflow_too_small_cap);
    RUN_TEST(test_bb_serialize_meta_openapi_root_close_overflow_null_out_len);
    RUN_TEST(test_bb_serialize_meta_openapi_root_close_success_null_out_len);

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

    RUN_TEST(test_bb_health_assemble_schema_root_offline_on_compose_failure);
    RUN_TEST(test_bb_health_assemble_schema_root_close_failure_returns_null);
    RUN_TEST(test_bb_health_assemble_schema_root_malloc_failure_returns_null);
    RUN_TEST(test_bb_health_assemble_schema_root_composes_matching_schema);
    RUN_TEST(test_bb_health_assemble_schema_root_idempotent_same_content);
    RUN_TEST(test_bb_health_assemble_schema_root_skips_section_with_null_schema_props);
    RUN_TEST(test_bb_health_assemble_schema_root_close_cap_override_no_shrink_when_larger);

    RUN_TEST(test_bb_display_info_topic_schema_offline_on_compose_failure);
    RUN_TEST(test_bb_display_info_topic_schema_matches_expected_content);
    RUN_TEST(test_bb_display_info_topic_schema_idempotent_pointer_stable);

    return UNITY_END();
}
