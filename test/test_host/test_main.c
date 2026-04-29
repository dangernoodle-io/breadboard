#include "unity.h"
#include "../../components/bb_log/src/bb_log_internal.h"
#include "bb_mdns_host_test_hooks.h"

// Forward declarations from test_bb_log.c
void test_bb_log_error(void);
void test_bb_log_warning(void);
void test_bb_log_info(void);
void test_bb_log_debug(void);
void test_bb_log_verbose(void);
void test_bb_log_zero_args(void);
void test_bb_log_level_from_str_error(void);
void test_bb_log_level_from_str_warn(void);
void test_bb_log_level_from_str_info(void);
void test_bb_log_level_from_str_debug(void);
void test_bb_log_level_from_str_verbose(void);
void test_bb_log_level_from_str_none(void);
void test_bb_log_level_from_str_case_insensitive(void);
void test_bb_log_level_from_str_invalid(void);
void test_bb_log_level_from_str_null(void);
void test_bb_log_level_set_noop(void);
void test_bb_log_tag_register_and_level(void);
void test_bb_log_tag_register_idempotent(void);
void test_bb_log_level_set_registers_tag(void);
void test_bb_log_level_set_updates_existing_tag(void);
void test_bb_log_tag_level_not_found(void);
void test_bb_log_tag_at_iteration(void);
void test_bb_log_level_to_str_none(void);
void test_bb_log_level_to_str_error(void);
void test_bb_log_level_to_str_warn(void);
void test_bb_log_level_to_str_info(void);
void test_bb_log_level_to_str_debug(void);
void test_bb_log_level_to_str_verbose(void);
void test_bb_log_level_to_str_unknown(void);
void test_bb_log_level_from_str_long_input(void);
void test_bb_log_level_set_null_tag(void);
void test_bb_log_tag_register_null_tag(void);
void test_bb_log_tag_level_null_args(void);
void test_bb_log_tag_at_null_args(void);
void test_bb_log_registry_full(void);

// Forward declarations from test_log_stream.c
void test_log_stream_format_basic(void);
void test_log_stream_format_truncation(void);
void test_log_stream_format_empty(void);
void test_log_stream_format_null_buf(void);
void test_log_stream_format_null_fmt(void);
void test_log_stream_format_int(void);
void test_log_stream_format_size_one(void);

// Forward declarations from test_ota_pull.c
void test_ota_pull_parse_version_found(void);
void test_ota_pull_parse_no_matching_asset(void);
void test_ota_pull_parse_empty_assets(void);
void test_ota_pull_parse_no_tag(void);
void test_ota_pull_parse_invalid_json(void);
void test_ota_pull_parse_multiple_assets(void);
void test_ota_pull_parse_tag_truncation(void);
void test_ota_pull_parse_url_truncation(void);
void test_ota_pull_parse_asset_missing_url(void);
void test_ota_pull_parse_assets_not_array(void);
void test_ota_pull_parse_null_inputs(void);
void test_ota_pull_parse_asset_url_null_value(void);
void test_ota_pull_parse_body_contains_tag_name_lookalike(void);
void test_ota_pull_parse_body_with_escapes_after_assets(void);
void test_ota_pull_parse_match_after_skipped_assets(void);
void test_ota_pull_skip_check_callback_registration(void);
void test_ota_pull_skip_check_callback_returns_true(void);
void test_ota_pull_skip_check_callback_returns_false(void);
void test_bb_ota_pull_set_http_timeout_ms_default_is_20000(void);
void test_bb_ota_pull_set_http_timeout_ms_zero_restores_default(void);

// Forward declarations from test_ota_push.c
void test_ota_push_skip_check_callback_registration(void);
void test_ota_push_skip_check_callback_returns_true(void);
void test_ota_push_skip_check_callback_returns_false(void);
void test_ota_push_hooks_registration(void);
void test_ota_push_pause_hook_returns_true(void);

// Forward declarations from test_http_utils.c
void test_url_decode_basic(void);
void test_url_decode_plus_as_space(void);
void test_url_decode_hex_decode(void);
void test_url_decode_missing_field(void);
void test_url_decode_truncation(void);
void test_url_decode_percent_at_end(void);
void test_url_decode_field_not_first(void);
void test_url_decode_empty_value(void);
void test_url_decode_field_at_end(void);
void test_prov_parse_empty_body(void);
void test_prov_parse_missing_ssid(void);
void test_prov_parse_ssid_only(void);
void test_prov_parse_ssid_and_pass(void);
void test_prov_parse_urlencoded_special(void);

// Forward declarations from test_bb_http_send_json.c
void test_bb_json_get_kind_object(void);
void test_bb_json_get_kind_array(void);
void test_bb_json_get_kind_scalar(void);
void test_bb_json_walk_children_object(void);
void test_bb_json_walk_children_array(void);
void test_bb_http_resp_send_json_callable(void);

// Forward declarations from test_nv_config.c
void test_nv_config_init_success(void);
void test_nv_config_wifi_ssid_empty_after_init(void);
void test_nv_config_wifi_pass_empty_after_init(void);
void test_nv_config_display_enabled_default_true(void);
void test_nv_config_is_provisioned_stub_returns_false(void);

// Forward declarations from test_ota_validator.c
void test_ota_validator_is_pending_false_on_host(void);
void test_ota_validator_mark_valid_returns_invalid_state_on_host(void);
void test_ota_validator_mark_valid_null_reason_on_host(void);
void test_ota_validator_mark_valid_idempotent_on_host(void);

// Forward declarations from test_nv_generic.c
void test_nv_set_u8_null_ns(void);
void test_nv_set_u8_null_key(void);
void test_nv_set_u8_valid(void);
void test_nv_set_u16_null_ns(void);
void test_nv_set_u16_null_key(void);
void test_nv_set_u16_valid(void);
void test_nv_set_u32_null_ns(void);
void test_nv_set_u32_null_key(void);
void test_nv_set_u32_valid(void);
void test_nv_set_str_null_ns(void);
void test_nv_set_str_null_key(void);
void test_nv_set_str_null_value(void);
void test_nv_set_str_valid(void);
void test_nv_get_u8_null_ns(void);
void test_nv_get_u8_null_key(void);
void test_nv_get_u8_null_out(void);
void test_nv_get_u8_returns_fallback(void);
void test_nv_get_u16_null_ns(void);
void test_nv_get_u16_null_key(void);
void test_nv_get_u16_null_out(void);
void test_nv_get_u16_returns_fallback(void);
void test_nv_get_u32_null_ns(void);
void test_nv_get_u32_null_key(void);
void test_nv_get_u32_null_out(void);
void test_nv_get_u32_returns_fallback(void);
void test_nv_get_str_null_ns(void);
void test_nv_get_str_null_key(void);
void test_nv_get_str_null_buf(void);
void test_nv_get_str_zero_len(void);
void test_nv_get_str_returns_fallback(void);
void test_nv_get_str_returns_fallback_null(void);
void test_nv_get_str_truncates_fallback(void);
void test_nv_erase_null_ns(void);
void test_nv_erase_null_key(void);
void test_nv_erase_valid(void);

// Forward declarations from test_bb_json.c
void test_bb_json_obj_string_roundtrip(void);
void test_bb_json_obj_number_roundtrip(void);
void test_bb_json_obj_bool_true_roundtrip(void);
void test_bb_json_obj_bool_false_roundtrip(void);
void test_bb_json_obj_null_roundtrip(void);
void test_bb_json_obj_multiple_fields(void);
void test_bb_json_nested_obj_in_obj(void);
void test_bb_json_arr_in_obj(void);
void test_bb_json_arr_of_strings(void);
void test_bb_json_arr_of_numbers(void);
void test_bb_json_arr_of_objects(void);
void test_bb_json_number_zero(void);
void test_bb_json_number_negative(void);
void test_bb_json_number_large(void);
void test_bb_json_number_decimal(void);
void test_bb_json_get_missing_string_returns_false(void);
void test_bb_json_get_missing_number_returns_false(void);
void test_bb_json_get_missing_bool_returns_false(void);
void test_bb_json_parse_invalid_returns_null(void);
void test_bb_json_parse_with_length(void);
void test_bb_json_serialize_null_returns_null(void);
void test_bb_json_type_mismatch_string_vs_number(void);
void test_bb_json_obj_get_item_returns_handle(void);
void test_bb_json_arr_size_and_get_item(void);
void test_bb_json_item_is_array_and_object(void);
void test_bb_json_item_serialize_subtree(void);
void test_bb_json_item_null_handle_is_safe(void);
void test_bb_json_arr_append_string_n_basic(void);
void test_bb_json_arr_append_string_n_null_arr_is_safe(void);
void test_bb_json_arr_append_string_n_null_str_is_safe(void);

// Forward declarations from test_route_registry.c
void test_route_registry_count_starts_at_zero(void);
void test_route_registry_add_increments_count(void);
void test_route_registry_add_two_increments_count(void);
void test_route_registry_foreach_visits_all(void);
void test_route_registry_foreach_preserves_insertion_order(void);
void test_route_registry_clear_empties_registry(void);
void test_route_registry_foreach_empty_is_noop(void);
void test_route_registry_foreach_null_cb_is_safe(void);
void test_route_registry_descriptor_fields_preserved(void);
void test_route_registry_count_after_clear_and_readd(void);
void test_register_described_route_rejects_null(void);
void test_register_described_route_propagates_underlying_failure(void);
void test_register_described_route_overflow_returns_ok(void);
void test_register_route_descriptor_only_overflow_logs_null_path(void);
void test_register_route_descriptor_only_rejects_null(void);
void test_register_route_descriptor_only_adds_to_registry(void);
void test_register_route_descriptor_only_overflow_returns_ok(void);
void test_http_route_handler_count_returns_zero_on_host(void);
void test_http_reserve_routes_accumulates(void);
void test_register_route_table_registers_all(void);
void test_register_route_table_null_table_returns_err(void);
void test_register_route_table_propagates_failure(void);

// Forward declarations from test_bb_http_assets.c
void test_asset_type_definition(void);
void test_asset_with_encoding(void);
void test_asset_table_definition(void);
void test_asset_path_mime_type_matching(void);
void test_asset_data_integrity(void);
void test_asset_null_encoding_absent(void);
void test_multiple_assets_different_types(void);
void test_asset_encoding_variations(void);
void test_zero_length_asset(void);

// Forward declarations from test_bb_wifi.c
void test_bb_wifi_set_hostname_null(void);
void test_bb_wifi_set_hostname_empty(void);
void test_bb_wifi_set_hostname_valid(void);

// Forward declarations from test_manifest.c
void test_manifest_empty_emits_empty_arrays(void);
void test_manifest_register_nv_single_namespace(void);
void test_manifest_register_nv_multiple_namespaces(void);
void test_manifest_nv_default_null_emits_json_null(void);
void test_manifest_nv_default_string_emits_json_string(void);
void test_manifest_nv_max_len_zero_omitted(void);
void test_manifest_register_mdns_single_service(void);
void test_manifest_register_mdns_multiple_services(void);
void test_manifest_mdns_values_null_omits_field(void);
void test_manifest_mdns_values_string_emits_array(void);
void test_manifest_register_nv_overflow_returns_err(void);
void test_manifest_register_mdns_overflow_returns_err(void);
void test_manifest_register_nv_too_many_keys_per_namespace(void);
void test_manifest_register_mdns_too_many_keys_per_service(void);
void test_manifest_register_nv_duplicate_namespace_returns_err(void);
void test_manifest_register_mdns_duplicate_service_returns_err(void);
void test_manifest_register_nv_null_namespace_returns_err(void);
void test_manifest_register_nv_null_keys_returns_err(void);
void test_manifest_register_nv_zero_keys_returns_err(void);
void test_manifest_register_mdns_null_service_returns_err(void);
void test_manifest_register_mdns_null_keys_returns_err(void);
void test_manifest_register_mdns_zero_keys_returns_err(void);
void test_manifest_emit_oom_root_object(void);
void test_manifest_emit_oom_nvs_array(void);
void test_manifest_emit_oom_namespace_object(void);
void test_manifest_emit_oom_keys_array(void);
void test_manifest_emit_oom_key_object(void);
void test_manifest_emit_oom_mdns_array(void);
void test_manifest_emit_oom_service_object(void);
void test_manifest_emit_oom_txt_array(void);
void test_manifest_emit_oom_txt_object(void);
void test_manifest_emit_oom_values_array(void);
void test_manifest_emit_oom_emit_values_arr_new(void);
void test_manifest_mdns_values_string_skips_empty_segments(void);

// Forward declarations from test_openapi_emit.c
void test_openapi_emit_openapi_version(void);
void test_openapi_emit_info_title(void);
void test_openapi_emit_paths_count(void);
void test_openapi_emit_foo_get_summary(void);
void test_openapi_emit_bar_post_request_body_schema_is_object(void);
void test_openapi_emit_foo_response_schema_is_object(void);
void test_openapi_emit_baz_derived_operation_id(void);
void test_openapi_emit_baz_no_tags_array(void);
void test_openapi_emit_null_meta_returns_null(void);
void test_openapi_emit_servers_present_when_url_set(void);
void test_openapi_emit_servers_absent_when_no_url(void);
void test_openapi_emit_patch_method_operation_id(void);
void test_openapi_emit_put_method_operation_id(void);
void test_openapi_emit_delete_method_operation_id(void);
void test_openapi_emit_options_method_operation_id(void);
void test_openapi_emit_derives_operation_id_with_dashes(void);
void test_openapi_emit_derives_operation_id_with_underscores(void);
void test_openapi_emit_derives_operation_id_without_api_prefix(void);
void test_openapi_emit_derives_operation_id_with_consecutive_slashes(void);
void test_openapi_emit_multiple_methods_same_path(void);
void test_openapi_emit_multiple_response_codes(void);
void test_openapi_emit_response_without_schema(void);
void test_openapi_emit_null_title_defaults_to_empty(void);
void test_openapi_emit_null_version_defaults_to_0_0_0(void);
void test_openapi_emit_null_description_omitted(void);
void test_openapi_emit_description_present_when_provided(void);
void test_openapi_emit_route_null_summary_omitted(void);
void test_openapi_emit_route_null_responses_array(void);
void test_openapi_emit_request_schema_without_content_type(void);
void test_openapi_emit_request_content_type_without_schema(void);
void test_openapi_emit_oom_root_alloc_returns_null(void);
void test_openapi_emit_oom_info_alloc_returns_null(void);
void test_openapi_emit_oom_servers_arr_skips(void);
void test_openapi_emit_oom_server_entry_frees_arr(void);
void test_openapi_emit_oom_paths_obj_returns_null(void);
void test_openapi_emit_oom_path_item_skips_path(void);
void test_openapi_emit_oom_op_alloc_skips_operation(void);
void test_openapi_emit_oom_tags_alloc_omits_tags(void);
void test_openapi_emit_oom_req_body_alloc_skips_request(void);
void test_openapi_emit_oom_req_content_alloc_frees_req_body(void);
void test_openapi_emit_oom_req_media_alloc_frees_req_body_and_content(void);
void test_openapi_emit_oom_responses_alloc_omits_responses(void);
void test_openapi_emit_oom_resp_obj_alloc_skips_response(void);
void test_openapi_emit_oom_resp_content_alloc_omits_content(void);
void test_openapi_emit_oom_resp_media_alloc_omits_content(void);
void test_openapi_emit_invalid_method_defaults_to_get(void);
void test_openapi_emit_response_null_description(void);
void test_openapi_emit_response_null_content_type_defaults_to_json(void);
void test_openapi_emit_long_path_truncates_operation_id(void);

// Forward declarations from test_bb_system.c
void test_bb_system_get_version_returns_nonnull(void);
void test_bb_system_get_version_default_is_host_string(void);
void test_bb_system_get_project_name_returns_nonnull_nonempty(void);
void test_bb_system_get_build_date_returns_nonnull_nonempty(void);
void test_bb_system_get_build_time_returns_nonnull_nonempty(void);
void test_bb_system_get_idf_version_returns_nonnull_nonempty(void);
void test_bb_error_check_happy_path(void);

// Forward declarations from test_bb_mdns.c
void test_bb_mdns_browse_start_null_service(void);
void test_bb_mdns_browse_start_null_proto(void);
void test_bb_mdns_browse_stop_unstarted(void);
void test_bb_mdns_browse_start_valid(void);
void test_bb_mdns_browse_stop_valid(void);
void test_bb_mdns_browse_stop_null_service(void);
void test_bb_mdns_browse_stop_null_proto(void);
void test_mdns_announce_explicit_increments_counter(void);
void test_mdns_set_txt_does_not_announce_immediately(void);
void test_mdns_set_txt_null_key_is_safe(void);
void test_mdns_set_txt_null_value_is_safe(void);
void test_mdns_host_reset_clears_counters(void);
void test_bb_mdns_dispatch_peer_fires_callback(void);
void test_bb_mdns_dispatch_removed_fires_callback(void);
void test_bb_mdns_dispatch_peer_null_cb_no_crash(void);
void test_bb_mdns_dispatch_removed_null_cb_no_crash(void);
void test_bb_mdns_dispatch_no_subscription_returns_ok(void);
void test_bb_mdns_dispatch_peer_with_empty_ip4_dispatches_anyway(void);
void test_bb_mdns_query_txt_null_args_returns_invalid_arg(void);
void test_bb_mdns_query_dispatch_invokes_cb_with_result(void);
void test_bb_mdns_query_dispatch_propagates_err_field(void);

// Forward declarations from test_bb_registry.c
void test_bb_registry_starts_empty(void);
void test_bb_registry_add_increments_count(void);
void test_bb_registry_foreach_visits_all_in_order(void);
void test_bb_registry_init_calls_each_init_fn(void);
void test_bb_registry_init_reports_first_error_but_continues(void);
void test_bb_registry_clear_resets_count(void);

// Forward declarations from test_bb_timer.c
void test_bb_timer_create_null_out_returns_err(void);
void test_bb_timer_create_null_cb_returns_err(void);
void test_bb_timer_one_shot_fires_once(void);
void test_bb_timer_periodic_fires_repeatedly_then_stops(void);
void test_bb_timer_delete_after_stop(void);
void test_bb_timer_delete_without_start(void);

// Forward declarations from test_bb_board.c
void test_bb_board_heap_free_total_callable(void);
void test_bb_board_heap_free_internal_callable(void);
void test_bb_board_heap_minimum_ever_callable(void);
void test_bb_board_heap_largest_free_block_callable(void);
void test_bb_board_chip_revision_callable(void);
void test_bb_board_cpu_freq_mhz_callable(void);

// Forward declarations from test_bb_info.c
void test_bb_health_register_extender_null_returns_err(void);
void test_bb_health_register_extender_capacity(void);
void test_bb_info_register_extender_null_returns_err(void);

// Forward declarations from test_wifi_reconn_policy.c
void wifi_reconn_policy_test_reset(void);
void test_wifi_reconn_tier1_handshake_fast_retry(void);
void test_wifi_reconn_tier2_handshake_backoff(void);
void test_wifi_reconn_tier3_handshake_backoff(void);
void test_wifi_reconn_generic_fast_retry(void);
void test_wifi_reconn_generic_backoff(void);
void test_wifi_reconn_5min_escape_hatch(void);
void test_wifi_reconn_got_ip_resets_counters(void);
void test_wifi_reconn_histogram_increments(void);
void test_wifi_reconn_state_reset(void);
void test_wifi_reconn_null_args_return_none(void);
void test_wifi_reconn_histogram_saturates_at_uint16_max(void);

// Forward declarations from test_bb_mdns_lifecycle.c
void bb_mdns_lifecycle_test_reset(void);
void test_bb_mdns_lifecycle_start_when_not_started(void);
void test_bb_mdns_lifecycle_start_when_already_started_is_noop(void);
void test_bb_mdns_lifecycle_start_init_failure_keeps_state_unstarted(void);
void test_bb_mdns_lifecycle_stop_when_started_sends_bye_then_free(void);
void test_bb_mdns_lifecycle_stop_when_not_started_is_noop(void);
void test_bb_mdns_lifecycle_announce_when_started_calls_apply(void);
void test_bb_mdns_lifecycle_announce_when_stopped_marks_dirty(void);
void test_bb_mdns_lifecycle_restart_cycle(void);
void test_bb_mdns_lifecycle_invalid_args(void);

void setUp(void) {
    _bb_log_registry_reset();
    bb_mdns_host_reset();
    wifi_reconn_policy_test_reset();
    bb_mdns_lifecycle_test_reset();
}
void tearDown(void) {}

int main(void) {
    UNITY_BEGIN();

    // bb_log macro tests
    RUN_TEST(test_bb_log_error);
    RUN_TEST(test_bb_log_warning);
    RUN_TEST(test_bb_log_info);
    RUN_TEST(test_bb_log_debug);
    RUN_TEST(test_bb_log_verbose);
    RUN_TEST(test_bb_log_zero_args);

    // bb_log level conversion tests
    RUN_TEST(test_bb_log_level_from_str_error);
    RUN_TEST(test_bb_log_level_from_str_warn);
    RUN_TEST(test_bb_log_level_from_str_info);
    RUN_TEST(test_bb_log_level_from_str_debug);
    RUN_TEST(test_bb_log_level_from_str_verbose);
    RUN_TEST(test_bb_log_level_from_str_none);
    RUN_TEST(test_bb_log_level_from_str_case_insensitive);
    RUN_TEST(test_bb_log_level_from_str_invalid);
    RUN_TEST(test_bb_log_level_from_str_null);
    RUN_TEST(test_bb_log_level_set_noop);
    RUN_TEST(test_bb_log_level_to_str_none);
    RUN_TEST(test_bb_log_level_to_str_error);
    RUN_TEST(test_bb_log_level_to_str_warn);
    RUN_TEST(test_bb_log_level_to_str_info);
    RUN_TEST(test_bb_log_level_to_str_debug);
    RUN_TEST(test_bb_log_level_to_str_verbose);
    RUN_TEST(test_bb_log_level_to_str_unknown);
    RUN_TEST(test_bb_log_level_from_str_long_input);
    RUN_TEST(test_bb_log_level_set_null_tag);
    RUN_TEST(test_bb_log_tag_register_null_tag);
    RUN_TEST(test_bb_log_tag_level_null_args);
    RUN_TEST(test_bb_log_tag_at_null_args);

    // bb_log registry tests
    RUN_TEST(test_bb_log_tag_register_and_level);
    RUN_TEST(test_bb_log_tag_register_idempotent);
    RUN_TEST(test_bb_log_level_set_registers_tag);
    RUN_TEST(test_bb_log_level_set_updates_existing_tag);
    RUN_TEST(test_bb_log_tag_level_not_found);
    RUN_TEST(test_bb_log_tag_at_iteration);
    RUN_TEST(test_bb_log_registry_full);

    // Log stream tests
    RUN_TEST(test_log_stream_format_basic);
    RUN_TEST(test_log_stream_format_truncation);
    RUN_TEST(test_log_stream_format_empty);
    RUN_TEST(test_log_stream_format_null_buf);
    RUN_TEST(test_log_stream_format_null_fmt);
    RUN_TEST(test_log_stream_format_int);
    RUN_TEST(test_log_stream_format_size_one);

    // OTA pull tests
    RUN_TEST(test_ota_pull_parse_version_found);
    RUN_TEST(test_ota_pull_parse_no_matching_asset);
    RUN_TEST(test_ota_pull_parse_empty_assets);
    RUN_TEST(test_ota_pull_parse_no_tag);
    RUN_TEST(test_ota_pull_parse_invalid_json);
    RUN_TEST(test_ota_pull_parse_multiple_assets);
    RUN_TEST(test_ota_pull_parse_tag_truncation);
    RUN_TEST(test_ota_pull_parse_url_truncation);
    RUN_TEST(test_ota_pull_parse_asset_missing_url);
    RUN_TEST(test_ota_pull_parse_assets_not_array);
    RUN_TEST(test_ota_pull_parse_null_inputs);
    RUN_TEST(test_ota_pull_parse_asset_url_null_value);
    RUN_TEST(test_ota_pull_parse_body_contains_tag_name_lookalike);
    RUN_TEST(test_ota_pull_parse_body_with_escapes_after_assets);
    RUN_TEST(test_ota_pull_parse_match_after_skipped_assets);
    RUN_TEST(test_ota_pull_skip_check_callback_registration);
    RUN_TEST(test_ota_pull_skip_check_callback_returns_true);
    RUN_TEST(test_ota_pull_skip_check_callback_returns_false);
    RUN_TEST(test_bb_ota_pull_set_http_timeout_ms_default_is_20000);
    RUN_TEST(test_bb_ota_pull_set_http_timeout_ms_zero_restores_default);

    // OTA push tests
    RUN_TEST(test_ota_push_skip_check_callback_registration);
    RUN_TEST(test_ota_push_skip_check_callback_returns_true);
    RUN_TEST(test_ota_push_skip_check_callback_returns_false);
    RUN_TEST(test_ota_push_hooks_registration);
    RUN_TEST(test_ota_push_pause_hook_returns_true);

    // OTA validator tests
    RUN_TEST(test_ota_validator_is_pending_false_on_host);
    RUN_TEST(test_ota_validator_mark_valid_returns_invalid_state_on_host);
    RUN_TEST(test_ota_validator_mark_valid_null_reason_on_host);
    RUN_TEST(test_ota_validator_mark_valid_idempotent_on_host);

    // HTTP utils tests
    RUN_TEST(test_url_decode_basic);
    RUN_TEST(test_url_decode_plus_as_space);
    RUN_TEST(test_url_decode_hex_decode);
    RUN_TEST(test_url_decode_missing_field);
    RUN_TEST(test_url_decode_truncation);
    RUN_TEST(test_url_decode_percent_at_end);
    RUN_TEST(test_url_decode_field_not_first);
    RUN_TEST(test_url_decode_empty_value);
    RUN_TEST(test_url_decode_field_at_end);
    RUN_TEST(test_prov_parse_empty_body);
    RUN_TEST(test_prov_parse_missing_ssid);
    RUN_TEST(test_prov_parse_ssid_only);
    RUN_TEST(test_prov_parse_ssid_and_pass);
    RUN_TEST(test_prov_parse_urlencoded_special);

    // JSON walker and HTTP send_json tests
    RUN_TEST(test_bb_json_get_kind_object);
    RUN_TEST(test_bb_json_get_kind_array);
    RUN_TEST(test_bb_json_get_kind_scalar);
    RUN_TEST(test_bb_json_walk_children_object);
    RUN_TEST(test_bb_json_walk_children_array);
    RUN_TEST(test_bb_http_resp_send_json_callable);

    // NV config tests
    RUN_TEST(test_nv_config_init_success);
    RUN_TEST(test_nv_config_wifi_ssid_empty_after_init);
    RUN_TEST(test_nv_config_wifi_pass_empty_after_init);
    RUN_TEST(test_nv_config_display_enabled_default_true);
    RUN_TEST(test_nv_config_is_provisioned_stub_returns_false);

    // Generic NV API tests
    RUN_TEST(test_nv_set_u8_null_ns);
    RUN_TEST(test_nv_set_u8_null_key);
    RUN_TEST(test_nv_set_u8_valid);
    RUN_TEST(test_nv_set_u16_null_ns);
    RUN_TEST(test_nv_set_u16_null_key);
    RUN_TEST(test_nv_set_u16_valid);
    RUN_TEST(test_nv_set_u32_null_ns);
    RUN_TEST(test_nv_set_u32_null_key);
    RUN_TEST(test_nv_set_u32_valid);
    RUN_TEST(test_nv_set_str_null_ns);
    RUN_TEST(test_nv_set_str_null_key);
    RUN_TEST(test_nv_set_str_null_value);
    RUN_TEST(test_nv_set_str_valid);
    RUN_TEST(test_nv_get_u8_null_ns);
    RUN_TEST(test_nv_get_u8_null_key);
    RUN_TEST(test_nv_get_u8_null_out);
    RUN_TEST(test_nv_get_u8_returns_fallback);
    RUN_TEST(test_nv_get_u16_null_ns);
    RUN_TEST(test_nv_get_u16_null_key);
    RUN_TEST(test_nv_get_u16_null_out);
    RUN_TEST(test_nv_get_u16_returns_fallback);
    RUN_TEST(test_nv_get_u32_null_ns);
    RUN_TEST(test_nv_get_u32_null_key);
    RUN_TEST(test_nv_get_u32_null_out);
    RUN_TEST(test_nv_get_u32_returns_fallback);
    RUN_TEST(test_nv_get_str_null_ns);
    RUN_TEST(test_nv_get_str_null_key);
    RUN_TEST(test_nv_get_str_null_buf);
    RUN_TEST(test_nv_get_str_zero_len);
    RUN_TEST(test_nv_get_str_returns_fallback);
    RUN_TEST(test_nv_get_str_returns_fallback_null);
    RUN_TEST(test_nv_get_str_truncates_fallback);
    RUN_TEST(test_nv_erase_null_ns);
    RUN_TEST(test_nv_erase_null_key);
    RUN_TEST(test_nv_erase_valid);

    // Route registry tests
    RUN_TEST(test_route_registry_count_starts_at_zero);
    RUN_TEST(test_route_registry_add_increments_count);
    RUN_TEST(test_route_registry_add_two_increments_count);
    RUN_TEST(test_route_registry_foreach_visits_all);
    RUN_TEST(test_route_registry_foreach_preserves_insertion_order);
    RUN_TEST(test_route_registry_clear_empties_registry);
    RUN_TEST(test_route_registry_foreach_empty_is_noop);
    RUN_TEST(test_route_registry_foreach_null_cb_is_safe);
    RUN_TEST(test_route_registry_descriptor_fields_preserved);
    RUN_TEST(test_route_registry_count_after_clear_and_readd);
    RUN_TEST(test_register_described_route_rejects_null);
    RUN_TEST(test_register_described_route_propagates_underlying_failure);
    RUN_TEST(test_register_described_route_overflow_returns_ok);
    RUN_TEST(test_register_route_descriptor_only_overflow_logs_null_path);
    RUN_TEST(test_register_route_descriptor_only_rejects_null);
    RUN_TEST(test_register_route_descriptor_only_adds_to_registry);
    RUN_TEST(test_register_route_descriptor_only_overflow_returns_ok);
    RUN_TEST(test_http_route_handler_count_returns_zero_on_host);
    RUN_TEST(test_http_reserve_routes_accumulates);
    RUN_TEST(test_register_route_table_registers_all);
    RUN_TEST(test_register_route_table_null_table_returns_err);
    RUN_TEST(test_register_route_table_propagates_failure);

    // HTTP asset tests
    RUN_TEST(test_asset_type_definition);
    RUN_TEST(test_asset_with_encoding);
    RUN_TEST(test_asset_table_definition);
    RUN_TEST(test_asset_path_mime_type_matching);
    RUN_TEST(test_asset_data_integrity);
    RUN_TEST(test_asset_null_encoding_absent);
    RUN_TEST(test_multiple_assets_different_types);
    RUN_TEST(test_asset_encoding_variations);
    RUN_TEST(test_zero_length_asset);

    // bb_json tests
    RUN_TEST(test_bb_json_obj_string_roundtrip);
    RUN_TEST(test_bb_json_obj_number_roundtrip);
    RUN_TEST(test_bb_json_obj_bool_true_roundtrip);
    RUN_TEST(test_bb_json_obj_bool_false_roundtrip);
    RUN_TEST(test_bb_json_obj_null_roundtrip);
    RUN_TEST(test_bb_json_obj_multiple_fields);
    RUN_TEST(test_bb_json_nested_obj_in_obj);
    RUN_TEST(test_bb_json_arr_in_obj);
    RUN_TEST(test_bb_json_arr_of_strings);
    RUN_TEST(test_bb_json_arr_of_numbers);
    RUN_TEST(test_bb_json_arr_of_objects);
    RUN_TEST(test_bb_json_number_zero);
    RUN_TEST(test_bb_json_number_negative);
    RUN_TEST(test_bb_json_number_large);
    RUN_TEST(test_bb_json_number_decimal);
    RUN_TEST(test_bb_json_get_missing_string_returns_false);
    RUN_TEST(test_bb_json_get_missing_number_returns_false);
    RUN_TEST(test_bb_json_get_missing_bool_returns_false);
    RUN_TEST(test_bb_json_parse_invalid_returns_null);
    RUN_TEST(test_bb_json_parse_with_length);
    RUN_TEST(test_bb_json_serialize_null_returns_null);
    RUN_TEST(test_bb_json_type_mismatch_string_vs_number);
    RUN_TEST(test_bb_json_obj_get_item_returns_handle);
    RUN_TEST(test_bb_json_arr_size_and_get_item);
    RUN_TEST(test_bb_json_item_is_array_and_object);
    RUN_TEST(test_bb_json_item_serialize_subtree);
    RUN_TEST(test_bb_json_item_null_handle_is_safe);
    RUN_TEST(test_bb_json_arr_append_string_n_basic);
    RUN_TEST(test_bb_json_arr_append_string_n_null_arr_is_safe);
    RUN_TEST(test_bb_json_arr_append_string_n_null_str_is_safe);

    // bb_wifi tests
    RUN_TEST(test_bb_wifi_set_hostname_null);
    RUN_TEST(test_bb_wifi_set_hostname_empty);
    RUN_TEST(test_bb_wifi_set_hostname_valid);

    // bb_manifest tests
    RUN_TEST(test_manifest_empty_emits_empty_arrays);
    RUN_TEST(test_manifest_register_nv_single_namespace);
    RUN_TEST(test_manifest_register_nv_multiple_namespaces);
    RUN_TEST(test_manifest_nv_default_null_emits_json_null);
    RUN_TEST(test_manifest_nv_default_string_emits_json_string);
    RUN_TEST(test_manifest_nv_max_len_zero_omitted);
    RUN_TEST(test_manifest_register_mdns_single_service);
    RUN_TEST(test_manifest_register_mdns_multiple_services);
    RUN_TEST(test_manifest_mdns_values_null_omits_field);
    RUN_TEST(test_manifest_mdns_values_string_emits_array);
    RUN_TEST(test_manifest_register_nv_overflow_returns_err);
    RUN_TEST(test_manifest_register_mdns_overflow_returns_err);
    RUN_TEST(test_manifest_register_nv_too_many_keys_per_namespace);
    RUN_TEST(test_manifest_register_mdns_too_many_keys_per_service);
    RUN_TEST(test_manifest_register_nv_duplicate_namespace_returns_err);
    RUN_TEST(test_manifest_register_mdns_duplicate_service_returns_err);
    RUN_TEST(test_manifest_register_nv_null_namespace_returns_err);
    RUN_TEST(test_manifest_register_nv_null_keys_returns_err);
    RUN_TEST(test_manifest_register_nv_zero_keys_returns_err);
    RUN_TEST(test_manifest_register_mdns_null_service_returns_err);
    RUN_TEST(test_manifest_register_mdns_null_keys_returns_err);
    RUN_TEST(test_manifest_register_mdns_zero_keys_returns_err);
    RUN_TEST(test_manifest_emit_oom_root_object);
    RUN_TEST(test_manifest_emit_oom_nvs_array);
    RUN_TEST(test_manifest_emit_oom_namespace_object);
    RUN_TEST(test_manifest_emit_oom_keys_array);
    RUN_TEST(test_manifest_emit_oom_key_object);
    RUN_TEST(test_manifest_emit_oom_mdns_array);
    RUN_TEST(test_manifest_emit_oom_service_object);
    RUN_TEST(test_manifest_emit_oom_txt_array);
    RUN_TEST(test_manifest_emit_oom_txt_object);
    RUN_TEST(test_manifest_emit_oom_values_array);
    RUN_TEST(test_manifest_emit_oom_emit_values_arr_new);
    RUN_TEST(test_manifest_mdns_values_string_skips_empty_segments);

    // bb_openapi emitter tests
    RUN_TEST(test_openapi_emit_openapi_version);
    RUN_TEST(test_openapi_emit_info_title);
    RUN_TEST(test_openapi_emit_paths_count);
    RUN_TEST(test_openapi_emit_foo_get_summary);
    RUN_TEST(test_openapi_emit_bar_post_request_body_schema_is_object);
    RUN_TEST(test_openapi_emit_foo_response_schema_is_object);
    RUN_TEST(test_openapi_emit_baz_derived_operation_id);
    RUN_TEST(test_openapi_emit_baz_no_tags_array);
    RUN_TEST(test_openapi_emit_null_meta_returns_null);
    RUN_TEST(test_openapi_emit_servers_present_when_url_set);
    RUN_TEST(test_openapi_emit_servers_absent_when_no_url);
    RUN_TEST(test_openapi_emit_patch_method_operation_id);
    RUN_TEST(test_openapi_emit_put_method_operation_id);
    RUN_TEST(test_openapi_emit_delete_method_operation_id);
    RUN_TEST(test_openapi_emit_options_method_operation_id);
    RUN_TEST(test_openapi_emit_derives_operation_id_with_dashes);
    RUN_TEST(test_openapi_emit_derives_operation_id_with_underscores);
    RUN_TEST(test_openapi_emit_derives_operation_id_without_api_prefix);
    RUN_TEST(test_openapi_emit_derives_operation_id_with_consecutive_slashes);
    RUN_TEST(test_openapi_emit_multiple_methods_same_path);
    RUN_TEST(test_openapi_emit_multiple_response_codes);
    RUN_TEST(test_openapi_emit_response_without_schema);
    RUN_TEST(test_openapi_emit_null_title_defaults_to_empty);
    RUN_TEST(test_openapi_emit_null_version_defaults_to_0_0_0);
    RUN_TEST(test_openapi_emit_null_description_omitted);
    RUN_TEST(test_openapi_emit_description_present_when_provided);
    RUN_TEST(test_openapi_emit_route_null_summary_omitted);
    RUN_TEST(test_openapi_emit_route_null_responses_array);
    RUN_TEST(test_openapi_emit_request_schema_without_content_type);
    RUN_TEST(test_openapi_emit_request_content_type_without_schema);
    RUN_TEST(test_openapi_emit_oom_root_alloc_returns_null);
    RUN_TEST(test_openapi_emit_oom_info_alloc_returns_null);
    RUN_TEST(test_openapi_emit_oom_servers_arr_skips);
    RUN_TEST(test_openapi_emit_oom_server_entry_frees_arr);
    RUN_TEST(test_openapi_emit_oom_paths_obj_returns_null);
    RUN_TEST(test_openapi_emit_oom_path_item_skips_path);
    RUN_TEST(test_openapi_emit_oom_op_alloc_skips_operation);
    RUN_TEST(test_openapi_emit_oom_tags_alloc_omits_tags);
    RUN_TEST(test_openapi_emit_oom_req_body_alloc_skips_request);
    RUN_TEST(test_openapi_emit_oom_req_content_alloc_frees_req_body);
    RUN_TEST(test_openapi_emit_oom_req_media_alloc_frees_req_body_and_content);
    RUN_TEST(test_openapi_emit_oom_responses_alloc_omits_responses);
    RUN_TEST(test_openapi_emit_oom_resp_obj_alloc_skips_response);
    RUN_TEST(test_openapi_emit_oom_resp_content_alloc_omits_content);
    RUN_TEST(test_openapi_emit_oom_resp_media_alloc_omits_content);
    RUN_TEST(test_openapi_emit_invalid_method_defaults_to_get);
    RUN_TEST(test_openapi_emit_response_null_description);
    RUN_TEST(test_openapi_emit_response_null_content_type_defaults_to_json);
    RUN_TEST(test_openapi_emit_long_path_truncates_operation_id);

    // bb_system tests
    RUN_TEST(test_bb_system_get_version_returns_nonnull);
    RUN_TEST(test_bb_system_get_version_default_is_host_string);
    RUN_TEST(test_bb_system_get_project_name_returns_nonnull_nonempty);
    RUN_TEST(test_bb_system_get_build_date_returns_nonnull_nonempty);
    RUN_TEST(test_bb_system_get_build_time_returns_nonnull_nonempty);
    RUN_TEST(test_bb_system_get_idf_version_returns_nonnull_nonempty);
    RUN_TEST(test_bb_error_check_happy_path);

    // bb_mdns tests
    RUN_TEST(test_bb_mdns_browse_start_null_service);
    RUN_TEST(test_bb_mdns_browse_start_null_proto);
    RUN_TEST(test_bb_mdns_browse_stop_unstarted);
    RUN_TEST(test_bb_mdns_browse_start_valid);
    RUN_TEST(test_bb_mdns_browse_stop_valid);
    RUN_TEST(test_bb_mdns_browse_stop_null_service);
    RUN_TEST(test_bb_mdns_browse_stop_null_proto);
    RUN_TEST(test_mdns_announce_explicit_increments_counter);
    RUN_TEST(test_mdns_set_txt_does_not_announce_immediately);
    RUN_TEST(test_mdns_set_txt_null_key_is_safe);
    RUN_TEST(test_mdns_set_txt_null_value_is_safe);
    RUN_TEST(test_mdns_host_reset_clears_counters);
    RUN_TEST(test_bb_mdns_dispatch_peer_fires_callback);
    RUN_TEST(test_bb_mdns_dispatch_removed_fires_callback);
    RUN_TEST(test_bb_mdns_dispatch_peer_null_cb_no_crash);
    RUN_TEST(test_bb_mdns_dispatch_removed_null_cb_no_crash);
    RUN_TEST(test_bb_mdns_dispatch_no_subscription_returns_ok);
    RUN_TEST(test_bb_mdns_dispatch_peer_with_empty_ip4_dispatches_anyway);
    RUN_TEST(test_bb_mdns_query_txt_null_args_returns_invalid_arg);
    RUN_TEST(test_bb_mdns_query_dispatch_invokes_cb_with_result);
    RUN_TEST(test_bb_mdns_query_dispatch_propagates_err_field);

    // bb_registry tests
    RUN_TEST(test_bb_registry_starts_empty);
    RUN_TEST(test_bb_registry_add_increments_count);
    RUN_TEST(test_bb_registry_foreach_visits_all_in_order);
    RUN_TEST(test_bb_registry_init_calls_each_init_fn);
    RUN_TEST(test_bb_registry_init_reports_first_error_but_continues);
    RUN_TEST(test_bb_registry_clear_resets_count);

    // bb_timer tests
    RUN_TEST(test_bb_timer_create_null_out_returns_err);
    RUN_TEST(test_bb_timer_create_null_cb_returns_err);
    RUN_TEST(test_bb_timer_one_shot_fires_once);
    RUN_TEST(test_bb_timer_periodic_fires_repeatedly_then_stops);
    RUN_TEST(test_bb_timer_delete_after_stop);
    RUN_TEST(test_bb_timer_delete_without_start);

    // bb_board tests
    RUN_TEST(test_bb_board_heap_free_total_callable);
    RUN_TEST(test_bb_board_heap_free_internal_callable);
    RUN_TEST(test_bb_board_heap_minimum_ever_callable);
    RUN_TEST(test_bb_board_heap_largest_free_block_callable);
    RUN_TEST(test_bb_board_chip_revision_callable);
    RUN_TEST(test_bb_board_cpu_freq_mhz_callable);

    // bb_info tests
    RUN_TEST(test_bb_health_register_extender_null_returns_err);
    RUN_TEST(test_bb_health_register_extender_capacity);
    RUN_TEST(test_bb_info_register_extender_null_returns_err);

    // wifi_reconn_policy tests
    RUN_TEST(test_wifi_reconn_tier1_handshake_fast_retry);
    RUN_TEST(test_wifi_reconn_tier2_handshake_backoff);
    RUN_TEST(test_wifi_reconn_tier3_handshake_backoff);
    RUN_TEST(test_wifi_reconn_generic_fast_retry);
    RUN_TEST(test_wifi_reconn_generic_backoff);
    RUN_TEST(test_wifi_reconn_5min_escape_hatch);
    RUN_TEST(test_wifi_reconn_got_ip_resets_counters);
    RUN_TEST(test_wifi_reconn_histogram_increments);
    RUN_TEST(test_wifi_reconn_state_reset);
    RUN_TEST(test_wifi_reconn_null_args_return_none);
    RUN_TEST(test_wifi_reconn_histogram_saturates_at_uint16_max);

    // bb_mdns_lifecycle tests
    RUN_TEST(test_bb_mdns_lifecycle_start_when_not_started);
    RUN_TEST(test_bb_mdns_lifecycle_start_when_already_started_is_noop);
    RUN_TEST(test_bb_mdns_lifecycle_start_init_failure_keeps_state_unstarted);
    RUN_TEST(test_bb_mdns_lifecycle_stop_when_started_sends_bye_then_free);
    RUN_TEST(test_bb_mdns_lifecycle_stop_when_not_started_is_noop);
    RUN_TEST(test_bb_mdns_lifecycle_announce_when_started_calls_apply);
    RUN_TEST(test_bb_mdns_lifecycle_announce_when_stopped_marks_dirty);
    RUN_TEST(test_bb_mdns_lifecycle_restart_cycle);
    RUN_TEST(test_bb_mdns_lifecycle_invalid_args);

    return UNITY_END();
}
