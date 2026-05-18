#include "unity.h"
#include "../../components/bb_log/src/bb_log_internal.h"
#include "bb_mdns_host_test_hooks.h"
#include "bb_event_test.h"
#include "../../components/bb_event_ring/bb_event_ring_internal.h"
#include "../../components/bb_event_routes/src/bb_event_routes_internal.h"
#include "../../platform/host/bb_http_client/bb_http_client_host.h"
#include "../../components/bb_update_check/src/bb_update_check_internal.h"
#include "test_alloc_inject.h"

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

// Forward declarations from test_bb_diag_panic.c
void test_bb_diag_panic_available_returns_false_on_host(void);
void test_bb_diag_panic_get_returns_not_found_on_host(void);
void test_bb_diag_panic_get_invalid_args(void);
void test_bb_diag_panic_clear_is_safe_on_host(void);
void test_bb_diag_panic_clear_after_unavailable(void);
void test_bb_diag_abnormal_reset_count_returns_zero_on_host(void);
void test_bb_diag_abnormal_reset_count_clear_is_safe_on_host(void);

// Forward declarations from test_ota_pull.c
void test_ota_pull_skip_check_callback_registration(void);
void test_ota_pull_skip_check_callback_returns_true(void);
void test_ota_pull_skip_check_callback_returns_false(void);
void test_bb_ota_pull_set_http_timeout_ms_default_is_20000(void);
void test_bb_ota_pull_set_http_timeout_ms_zero_restores_default(void);

// Forward declarations from test_bb_ota_pull_manifest.c
void test_ota_pull_manifest_fetch_success(void);
void test_ota_pull_manifest_transport_failure(void);
void test_ota_pull_manifest_http_404(void);
void test_ota_pull_manifest_board_asset_not_found(void);
void test_ota_pull_manifest_bad_json(void);
void test_ota_pull_manifest_realistic_github_payload(void);
void test_ota_pull_manifest_fallback_board_name(void);
void test_ota_pull_manifest_asset_found_ota_proceeds(void);

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
void test_bb_url_parse_bool_one(void);
void test_bb_url_parse_bool_zero(void);
void test_bb_url_parse_bool_true_lowercase(void);
void test_bb_url_parse_bool_true_uppercase(void);
void test_bb_url_parse_bool_false_lowercase(void);
void test_bb_url_parse_bool_on(void);
void test_bb_url_parse_bool_off(void);
void test_bb_url_parse_bool_yes(void);
void test_bb_url_parse_bool_no(void);
void test_bb_url_parse_bool_t(void);
void test_bb_url_parse_bool_f(void);
void test_bb_url_parse_bool_y(void);
void test_bb_url_parse_bool_n(void);
void test_bb_url_parse_bool_empty_string(void);
void test_bb_url_parse_bool_invalid_maybe(void);
void test_bb_url_parse_bool_invalid_two(void);
void test_bb_url_parse_bool_null_pointer(void);
void test_bb_url_parse_bool_prefix_of_valid(void);
void test_bb_url_parse_bool_extension_of_valid(void);
void test_bb_url_parse_uint_zero(void);
void test_bb_url_parse_uint_decimal_42(void);
void test_bb_url_parse_uint_max_unsigned_long(void);
void test_bb_url_parse_uint_empty_string(void);
void test_bb_url_parse_uint_non_digit(void);
void test_bb_url_parse_uint_trailing_junk(void);
void test_bb_url_parse_uint_leading_space(void);
void test_bb_url_parse_uint_negative_sign(void);
void test_bb_url_parse_uint_decimal_point(void);
void test_bb_url_parse_uint_null_pointer(void);
void test_bb_url_parse_uint_overflow(void);
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

// Forward declarations from test_bb_http_json_arr_stream.c
void test_json_arr_begin_null_req(void);
void test_json_arr_begin_null_stream(void);
void test_json_arr_begin_init(void);
void test_json_arr_emit_unopened(void);
void test_json_arr_emit_after_end(void);
void test_json_arr_sticky_error(void);
void test_json_arr_empty(void);
void test_json_arr_single_item(void);
void test_json_arr_multiple_items(void);
void test_json_arr_emit_null_item(void);
void test_json_arr_emit_null_stream(void);
void test_json_arr_end_null_stream(void);

// Forward declarations from test_nv_config.c
void test_nv_config_init_success(void);
void test_nv_config_wifi_ssid_empty_after_init(void);
void test_nv_config_wifi_pass_empty_after_init(void);
void test_nv_config_display_enabled_default_true(void);
void test_nv_config_is_provisioned_stub_returns_false(void);
void test_hostname_default_empty(void);
void test_hostname_set_get_roundtrip(void);
void test_hostname_validates_charset(void);
void test_hostname_validates_length(void);
void test_hostname_rejects_leading_hyphen(void);
void test_hostname_rejects_trailing_hyphen(void);
void test_hostname_rejects_null(void);

// Forward declarations from test_ota_validator.c
void test_ota_validator_is_pending_false_on_host(void);
void test_ota_validator_mark_valid_returns_invalid_state_on_host(void);
void test_ota_validator_mark_valid_null_reason_on_host(void);
void test_ota_validator_mark_valid_idempotent_on_host(void);

// Forward declarations from test_bb_release_manifest.c
void test_bb_release_manifest_parse_github_valid_manifest(void);
void test_bb_release_manifest_parse_github_missing_tag(void);
void test_bb_release_manifest_parse_github_missing_asset(void);
void test_bb_release_manifest_parse_github_null_args(void);
void test_bb_release_manifest_parse_github_bad_json(void);
void test_bb_release_manifest_parse_github_multiple_assets(void);
void test_bb_release_manifest_parse_github_whitespace_around_colons(void);
void test_bb_release_manifest_parse_github_handles_escape_sequences(void);
void test_bb_release_manifest_parse_github_unicode_escape_skipped(void);
void test_bb_release_manifest_parse_github_skips_non_matching_first_asset(void);
void test_bb_release_manifest_parse_github_asset_missing_name_skipped(void);
void test_bb_release_manifest_parse_github_matching_asset_no_url(void);
void test_bb_release_manifest_parse_github_assets_not_array(void);
void test_bb_release_manifest_parse_github_empty_assets_array(void);

// Forward declarations from test_release_manifest_github_stream.c
void test_stream_begin_null_ctx_returns_invalid_arg(void);
void test_stream_begin_null_board_returns_invalid_arg(void);
void test_stream_begin_null_tag_returns_invalid_arg(void);
void test_stream_begin_null_url_returns_invalid_arg(void);
void test_stream_begin_zero_tag_cap_returns_invalid_arg(void);
void test_stream_begin_zero_url_cap_returns_invalid_arg(void);
void test_stream_begin_cap_of_one_returns_invalid_arg(void);
void test_stream_end_null_ctx_returns_invalid_arg(void);
void test_stream_whole_body_chunk(void);
void test_stream_256_byte_chunks(void);
void test_stream_7_byte_chunks(void);
void test_stream_1_byte_chunks(void);
void test_stream_skips_non_matching_first_asset(void);
void test_stream_skips_non_matching_first_asset_256(void);
void test_stream_missing_tag_returns_not_found(void);
void test_stream_missing_assets_returns_not_found(void);
void test_stream_no_matching_asset_returns_not_found(void);
void test_stream_empty_assets_array_returns_not_found(void);
void test_stream_bad_json_returns_not_found(void);
void test_stream_asset_missing_url_returns_not_found(void);
void test_stream_taipanminer_board_name(void);
void test_stream_backslash_slash_in_url(void);
void test_stream_feed_after_error_is_noop(void);
void test_stream_whitespace_around_colons_1byte(void);
void test_stream_backslash_backslash_in_url(void);
void test_stream_unicode_escape_in_url_dropped(void);
void test_stream_escape_in_asset_name_no_match(void);
void test_stream_board_name_truncation(void);
void test_stream_skip_string_with_escape(void);
void test_stream_toplevel_nested_array_skipped(void);
void test_stream_toplevel_nested_object_skipped(void);
void test_stream_skip_depth_string_with_braces(void);
void test_stream_skip_depth_string_with_escape(void);
void test_stream_escaped_key_char_ignored(void);
void test_stream_assets_array_exit_via_scan_key(void);
void test_stream_asset_scalar_field_skipped(void);
void test_stream_toplevel_scalar_field_skipped(void);
void test_stream_asset_with_nested_array_field_skipped(void);
void test_stream_asset_with_nested_object_field_skipped(void);
void test_stream_asset_name_at_capacity_truncated_no_match(void);
void test_stream_very_long_key_truncated_ignored(void);
void test_stream_long_key_with_escape_at_capacity_ignored(void);
void test_stream_skip_depth_nested_array_inside_object(void);
void test_stream_skip_depth_multi_level_nesting(void);

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
void test_nv_batch_begin_null_batch(void);
void test_nv_batch_begin_null_ns(void);
void test_nv_batch_begin_valid(void);
void test_nv_batch_set_u32_before_begin(void);
void test_nv_batch_set_u32_null_key(void);
void test_nv_batch_set_u32_valid(void);
void test_nv_batch_set_str_null_value(void);
void test_nv_batch_commit_null(void);
void test_nv_batch_set_after_commit_rejected(void);
void test_nv_batch_three_u32_writes_succeed(void);

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
// Forward declarations from test_route_schemas.c
void test_route_schemas_manifest_fixture_parses(void);
void test_route_schemas_registry_all_valid(void);
void test_route_schemas_walker_flags_malformed(void);
void test_set_raw_writes_null_on_malformed_json(void);
void test_set_raw_writes_parsed_object_on_valid_json(void);
// Forward declarations from test_route_fidelity.c
void test_fidelity_reboot(void);
void test_fidelity_board(void);
void test_fidelity_info(void);
void test_fidelity_health(void);
void test_fidelity_wifi_info(void);
void test_fidelity_ota_status(void);
void test_fidelity_ota_mark_valid_409(void);
void test_capture_begin_end_basic(void);
void test_capture_default_status_is_200(void);
void test_capture_send_json_sets_content_type(void);
void test_capture_multi_write_appends(void);
void test_capture_end_null_args_returns_err(void);
void test_capture_no_active_slot_ignored(void);
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
void test_openapi_emit_parameters_array_present(void);
void test_openapi_emit_parameters_absent_when_null(void);
void test_openapi_emit_param_null_description_omitted(void);
void test_openapi_emit_param_in_path(void);
void test_openapi_emit_multiple_params_on_route(void);
void test_openapi_emit_param_count_zero_omits_parameters(void);
void test_openapi_emit_param_null_name_defaults_to_empty(void);
void test_openapi_emit_param_null_in_defaults_to_query(void);
void test_openapi_emit_oom_params_arr_skips_parameters(void);
void test_openapi_emit_oom_param_obj_skips_entry(void);
void test_openapi_emit_oom_schema_obj_skips_schema(void);

// Forward declarations from test_bb_openapi_validate.c
void test_validate_null_schema_json_returns_invalid_arg(void);
void test_validate_null_value_returns_invalid_arg(void);
void test_validate_malformed_schema_returns_invalid_arg(void);
void test_validate_type_string_match(void);
void test_validate_type_string_mismatch(void);
void test_validate_type_integer_match(void);
void test_validate_type_integer_mismatch(void);
void test_validate_type_boolean_match(void);
void test_validate_type_boolean_mismatch(void);
void test_validate_type_object_match(void);
void test_validate_type_object_mismatch(void);
void test_validate_type_array_match(void);
void test_validate_type_array_mismatch(void);
void test_validate_err_null_still_returns_validation_code(void);
void test_validate_required_present(void);
void test_validate_required_missing(void);
void test_validate_properties_nested_ok(void);
void test_validate_properties_nested_type_mismatch(void);
void test_validate_properties_deeply_nested(void);
void test_validate_items_all_ok(void);
void test_validate_items_element_mismatch(void);
void test_validate_items_nested_object_element(void);
void test_validate_enum_match(void);
void test_validate_enum_mismatch(void);
void test_validate_additional_properties_false_ok(void);
void test_validate_additional_properties_false_rejects_extra(void);
void test_validate_additional_properties_default_allows_extra(void);
void test_validate_unknown_keyword_does_not_fail(void);
void test_validate_unknown_type_value_passes(void);
void test_validate_enum_numeric_mismatch_renders_value(void);
void test_validate_smoke_reboot_schema(void);
void test_validate_smoke_ota_check_schema(void);
void test_validate_smoke_log_level_schema(void);
void test_validate_smoke_panic_schema(void);
void test_validate_enum_mismatch_null_err(void);
void test_validate_required_missing_null_err(void);
void test_validate_additional_properties_rejection_null_err(void);
void test_validate_malformed_required_non_array_ignored(void);
void test_validate_required_non_string_item_skipped(void);
void test_validate_malformed_properties_non_object_ignored(void);
void test_validate_malformed_type_non_string_ignored(void);
void test_validate_malformed_enum_non_array_ignored(void);
void test_validate_required_non_object_value_skipped(void);
void test_validate_properties_non_object_value_skipped(void);
void test_validate_additional_properties_true_allows_extra(void);
void test_validate_items_non_array_value_skipped(void);
void test_validate_enum_empty_always_fails(void);
void test_validate_enum_bool_value_renders_non_string(void);
void test_validate_enum_null_value_renders_non_string(void);
void test_validate_schema_not_object_passes(void);
void test_validate_schema_array_not_object_passes(void);
void test_validate_deep_path_object_array_object(void);
void test_validate_path_stack_overflow_does_not_crash(void);
void test_validate_type_number_match(void);
void test_validate_type_number_mismatch(void);
void test_validate_type_null_match(void);
void test_validate_type_null_mismatch(void);
void test_validate_path_render_truncation(void);
void test_validate_path_render_buffer_full_midloop(void);
void test_validate_path_push_index_overflow(void);
void test_validate_additional_properties_non_bool_ignored(void);

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
void test_bb_registry_init_honors_order_priority(void);
void test_bb_registry_init_same_order_preserves_insertion_order(void);
void test_bb_registry_init_order_mixed(void);
void test_bb_registry_pre_http_starts_empty(void);
void test_bb_registry_pre_http_add_increments_count(void);
void test_bb_registry_pre_http_foreach_visits_in_insertion_order(void);
void test_bb_registry_pre_http_init_calls_each_fn(void);
void test_bb_registry_pre_http_init_reports_first_error_but_continues(void);
void test_bb_registry_pre_http_clear_resets_count(void);

// Forward declarations from test_bb_byte_order.c
void test_bb_load_be32_constant(void);
void test_bb_load_le32_constant(void);
void test_bb_store_be32_round_trip(void);
void test_bb_store_le32_round_trip(void);
void test_bb_load_be32_store_be32_round_trip(void);
void test_bb_load_le32_store_le32_round_trip(void);
void test_bb_load_be16_constant(void);
void test_bb_load_le16_constant(void);
void test_bb_load_be16_store_be16_round_trip(void);
void test_bb_load_le16_store_le16_round_trip(void);
void test_bb_load_be32_misaligned(void);
void test_bb_store_be32_misaligned(void);
void test_bb_load_be16_misaligned(void);
void test_bb_store_be16_misaligned(void);

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

// Forward declarations from test_bb_led.c
void bb_led_test_reset(void);
void test_bb_led_caps_and_count(void);
void test_bb_led_set_on(void);
void test_bb_led_set_color_unsupported_when_no_rgb_cap(void);
void test_bb_led_idx_out_of_range(void);
void test_bb_led_fill_color_iterates(void);
void test_bb_led_close_calls_driver(void);
void test_bb_led_brightness_pct_validation(void);

// Forward declarations from test_bb_led_gpio.c
void test_gpio_open_close(void);
void test_gpio_active_high_set_on(void);
void test_gpio_active_low_set_on(void);
void test_gpio_idx_must_be_zero(void);
void test_gpio_initial_state_off(void);
void test_gpio_initial_state_off_active_low(void);
void test_gpio_invalid_args(void);

// Forward declarations from test_bb_led_pwm.c
void bb_led_pwm_test_reset(void);
void test_pwm_open_close(void);
void test_pwm_set_brightness_active_high(void);
void test_pwm_set_brightness_active_low(void);
void test_pwm_set_on_active_high(void);
void test_pwm_set_on_active_low(void);
void test_pwm_set_color_unsupported(void);
void test_pwm_idx_must_be_zero(void);
void test_pwm_invalid_args(void);
void test_pwm_initial_state_off_active_high(void);
void test_pwm_initial_state_off_active_low(void);

// Forward declarations from test_bb_led_apa102.c
void bb_led_apa102_host_test_reset(void);
void test_apa102_open_close(void);
void test_apa102_initial_flush_dark(void);
void test_apa102_set_color_and_flush(void);
void test_apa102_set_brightness_partial(void);
void test_apa102_fill_color(void);
void test_apa102_idx_out_of_range(void);
void test_apa102_invalid_args(void);
void test_apa102_disabled_pixel_zeros_rgb(void);

// Forward declarations from test_bb_button.c
void test_bb_button_open_null_cfg(void);
void test_bb_button_open_null_out(void);
void test_bb_button_press_past_debounce_fires_cb(void);
void test_bb_button_second_press_within_debounce_suppressed(void);
void test_bb_button_press_then_release_fires_two_cbs(void);
void test_bb_button_is_pressed_reflects_debounced_state(void);
void test_bb_button_get_queue_returns_null_on_host(void);
void test_bb_button_active_low_false_high_is_press(void);
void test_bb_button_close_subsequent_inject_noop(void);

// Forward declarations from test_bb_button_gpio.c
void test_btn_gpio_open_close(void);
void test_btn_gpio_initial_state_not_pressed(void);
void test_btn_gpio_inject_sets_pressed(void);
void test_btn_gpio_inject_sets_released(void);
void test_btn_gpio_invalid_args(void);
void test_btn_gpio_poll_noop_on_host(void);

// Forward declarations from test_bb_button_events.c
void test_btn_evt_attach_null_cfg_returns_invalid_arg(void);
void test_btn_evt_attach_null_out_returns_invalid_arg(void);
void test_btn_evt_attach_null_button_returns_invalid_arg(void);
void test_btn_evt_single_click_emits_exactly_one_click(void);
void test_btn_evt_double_click_emits_exactly_one_double_click(void);
void test_btn_evt_double_click_no_click_emitted(void);
void test_btn_evt_long_press_start_fires_once(void);
void test_btn_evt_repeat_events_monotonic_held_ms(void);
void test_btn_evt_long_press_end_correct_held_ms(void);
void test_btn_evt_no_repeat_after_long_press_end(void);
void test_btn_evt_medium_press_no_event(void);
void test_btn_evt_detach_no_crash_on_subsequent_events(void);

// Forward declarations from test_bb_event.c
void test_bb_event_init_topic_register_subscribe_post_pump_fires(void);
void test_bb_event_two_subscribers_both_receive(void);
void test_bb_event_unsubscribe_prevents_future_events(void);
void test_bb_event_post_exceeds_max_payload_returns_invalid_arg(void);
void test_bb_event_queue_overflow_returns_no_space(void);
void test_bb_event_topic_lookup_returns_same_handle(void);
void test_bb_event_topic_register_duplicate_returns_same_handle(void);
void test_bb_event_different_topics_dont_cross(void);
void test_bb_event_payload_integrity(void);
void test_bb_event_id_parameter_preserved(void);
void test_bb_event_init_null_cfg_uses_defaults(void);
void test_bb_event_init_idempotent(void);
void test_bb_event_topic_register_null_name_returns_invalid_arg(void);
void test_bb_event_topic_register_null_out_returns_invalid_arg(void);
void test_bb_event_topic_register_returns_ok_when_initialized(void);
void test_bb_event_topic_register_exceeds_max_returns_no_space(void);
void test_bb_event_topic_lookup_null_name_returns_invalid_arg(void);
void test_bb_event_topic_lookup_null_out_returns_invalid_arg(void);
void test_bb_event_topic_lookup_not_found(void);
void test_bb_event_subscribe_null_topic_returns_invalid_arg(void);
void test_bb_event_subscribe_null_callback_returns_invalid_arg(void);
void test_bb_event_subscribe_null_out_returns_invalid_arg(void);
void test_bb_event_unsubscribe_null_returns_invalid_arg(void);
void test_bb_event_post_null_topic_returns_invalid_arg(void);
void test_bb_event_post_payload_too_large_returns_invalid_arg(void);
void test_bb_event_post_with_small_payload_succeeds(void);
void test_bb_event_init_cfg_with_nonzero_values(void);
void test_bb_event_post_zero_payload_no_data(void);
void test_bb_event_unsubscribe_early_in_list(void);
void test_init_pool_guard_returns_early(void);
void test_init_with_zero_queue_depth_uses_default(void);
void test_init_with_zero_max_payload_uses_default(void);
void test_init_port_init_failure_returns_error(void);
void test_topic_register_before_init_returns_invalid_state(void);
void test_unsubscribe_non_head_subscriber(void);
void test_dispatch_null_entry_no_crash(void);
void test_bb_event_topic_register_walks_existing_entries(void);
void test_bb_event_topic_register_returns_no_space_when_full(void);
void test_bb_event_topic_lookup_walks_past_non_matches(void);
void test_bb_event_post_exceeds_max_payload_at_runtime_limit(void);

// Forward declarations from test_bb_event_ring.c
void test_bb_event_ring_attach_and_post_replay_delivers_all_entries(void);
void test_bb_event_ring_capacity_overflow_evicts_oldest(void);
void test_bb_event_ring_live_events_fire_after_subscribe(void);
void test_bb_event_ring_detach_stops_capturing(void);
void test_bb_event_ring_payload_integrity(void);
void test_bb_event_ring_attach_null_topic_returns_invalid_arg(void);
void test_bb_event_ring_attach_zero_capacity_returns_invalid_arg(void);
void test_bb_event_ring_attach_zero_max_entry_returns_invalid_arg(void);
void test_bb_event_ring_attach_null_out_returns_invalid_arg(void);
void test_bb_event_ring_subscribe_null_ring_returns_invalid_arg(void);
void test_bb_event_ring_subscribe_null_callback_returns_invalid_arg(void);
void test_bb_event_ring_subscribe_null_out_returns_invalid_arg(void);
void test_bb_event_ring_detach_null_noop(void);
void test_bb_event_ring_head_wraps_modulo_capacity(void);
void test_bb_event_ring_zero_payload_capture(void);
void test_bb_event_ring_empty_ring_replay(void);
void test_bb_event_ring_payload_with_data(void);
void test_ring_capture_with_size_nonzero_data_null(void);
void test_ring_attach_struct_calloc_fails(void);
void test_ring_attach_entries_calloc_fails(void);
void test_ring_attach_payload_calloc_fails(void);
void test_ring_subscribe_with_replay_snapshot_calloc_fails(void);
void test_ring_subscribe_when_subscriber_pool_exhausted(void);
void test_bb_event_ring_capture_null_data_with_size(void);
void test_bb_event_ring_attach_subscribe_failure_frees_all(void);
void test_bb_event_ring_subscribe_replay_second_alloc_failure_frees_first(void);

// Forward declarations from test_bb_event_ring_retained.c
void test_bb_event_ring_attach_ex_retained_true_returns_ok(void);
void test_bb_event_ring_attach_ex_retained_false_same_as_attach(void);
void test_bb_event_ring_retained_subscribe_after_one_post_replays(void);
void test_bb_event_ring_retained_capacity1_overflow_delivers_most_recent(void);
void test_bb_event_ring_retained_overflow_delivers_most_recent_n(void);
void test_bb_event_ring_attach_ex_null_topic_returns_invalid_arg(void);
void test_bb_event_ring_attach_ex_null_out_returns_invalid_arg(void);

// Forward declarations from test_bb_event_ring_introspection.c
void test_bb_event_ring_capacity_null_returns_zero(void);
void test_bb_event_ring_capacity_returns_configured_value(void);
void test_bb_event_ring_count_null_returns_zero(void);
void test_bb_event_ring_count_empty_ring_returns_zero(void);
void test_bb_event_ring_count_after_posts(void);
void test_bb_event_ring_count_capped_at_capacity(void);
void test_bb_event_ring_last_entry_info_null_ring_returns_invalid_arg(void);
void test_bb_event_ring_last_entry_info_empty_ring_returns_not_found(void);
void test_bb_event_ring_last_entry_info_populated_ring(void);
void test_bb_event_ring_last_entry_info_reflects_latest_post(void);
void test_bb_event_ring_last_entry_info_after_eviction(void);
void test_bb_event_ring_last_entry_info_null_out_params_ok(void);
void test_bb_event_ring_last_entry_info_zero_size_payload(void);

void test_bb_event_subscribe_with_prep_runs_prep_before_subscribe(void);
void test_bb_event_subscribe_with_prep_null_prep_subscribes(void);
void test_bb_event_subscribe_with_prep_invalid_args(void);
void test_bb_event_lock_unlock_round_trip(void);

// Forward declarations from test_bb_event_routes.c
// Forward declarations from test_bb_http_client.c
void test_bb_http_client_get_null_url_returns_invalid_arg(void);
void test_bb_http_client_get_null_body_returns_invalid_arg(void);
void test_bb_http_client_get_null_out_returns_invalid_arg(void);
void test_bb_http_client_get_zero_cap_returns_invalid_arg(void);
void test_bb_http_client_get_no_mock_returns_invalid_state(void);
void test_bb_http_client_get_mock_success_returns_body(void);
void test_bb_http_client_get_mock_404_returns_ok_with_status(void);
void test_bb_http_client_get_mock_truncates_when_body_too_big(void);
void test_bb_http_client_get_mock_transport_error_returns_passthrough(void);
void test_bb_http_client_get_empty_body_is_valid(void);
void test_bb_http_client_get_cfg_honored(void);

// Forward declarations from test_bb_http_client_stream.c
void test_bb_http_client_stream_null_url_returns_invalid_arg(void);
void test_bb_http_client_stream_null_cb_returns_invalid_arg(void);
void test_bb_http_client_stream_null_out_returns_invalid_arg(void);
void test_bb_http_client_stream_transport_error_propagated(void);
void test_bb_http_client_stream_small_body_reassembled(void);
void test_bb_http_client_stream_large_body_multiple_chunks(void);
void test_bb_http_client_stream_empty_body(void);
void test_bb_http_client_stream_early_stop_sets_truncated(void);
void test_bb_http_client_stream_cb_error_propagated(void);
void test_bb_http_client_stream_404_status_code(void);
void test_bb_http_client_stream_cfg_honored(void);

// Forward declarations from test_bb_update_check.c
void test_bb_update_check_init_idempotent(void);
void test_bb_update_check_init_with_cfg_uses_overrides(void);
void test_bb_update_check_get_status_before_init_returns_invalid_state(void);
void test_bb_update_check_get_status_null_out_returns_invalid_arg(void);
void test_bb_update_check_set_releases_url_validates(void);
void test_bb_update_check_set_releases_url_before_init_returns_invalid_state(void);
void test_bb_update_check_set_parser_before_init_returns_invalid_state(void);
void test_bb_update_check_set_parser_null_restores_default(void);
void test_bb_update_check_run_one_before_init_returns_invalid_arg(void);
void test_bb_update_check_run_one_without_url_returns_invalid_state(void);
void test_bb_update_check_now_without_url_returns_invalid_state(void);
void test_bb_update_check_now_before_init_returns_invalid_arg(void);
void test_bb_update_check_run_one_newer_release_flips_available(void);
void test_bb_update_check_run_one_same_version_keeps_unavailable(void);
void test_bb_update_check_run_one_transport_failure_sticky(void);
void test_bb_update_check_run_one_http_404_sticky_failure(void);
void test_bb_update_check_run_one_parse_failure_sticky(void);
void test_bb_update_check_run_one_recovers_after_failure(void);
void test_bb_update_check_run_one_custom_parser_invoked(void);
void test_bb_update_check_now_drives_a_check(void);
void test_bb_update_check_post_initial_publishes_on_first_check(void);
void test_bb_update_check_dev_tag_treated_as_older(void);
void test_bb_update_check_run_one_newer_to_same_transitions_back(void);
void test_bb_update_check_custom_parser_transport_error(void);
void test_bb_update_check_custom_parser_parse_failure(void);
void test_bb_update_check_custom_parser_http_404(void);
void test_bb_update_check_custom_parser_body_exceeds_16k(void);
void test_bb_update_check_custom_parser_post_initial_publishes(void);
void test_bb_update_check_set_hooks_before_init_returns_invalid_state(void);
void test_bb_update_check_set_hooks_null_clears(void);
void test_bb_update_check_hooks_called_in_order_on_success(void);
void test_bb_update_check_hooks_resume_fires_on_transport_error(void);
void test_bb_update_check_hooks_resume_fires_on_parse_error(void);
void test_bb_update_check_hooks_called_once_per_run(void);
void test_bb_update_check_hooks_custom_parser_success(void);
void test_bb_update_check_hooks_custom_parser_transport_error(void);
void test_bb_update_check_hooks_custom_parser_parse_error(void);
void test_bb_update_check_pause_returns_false_skips_fetch(void);
void test_bb_update_check_pause_returns_false_custom_parser_skips_fetch(void);
void test_bb_update_check_set_firmware_board_before_init_returns_invalid_state(void);
void test_bb_update_check_set_firmware_board_too_long_returns_invalid_arg(void);
void test_bb_update_check_set_firmware_board_null_clears_to_default(void);
void test_bb_update_check_set_firmware_board_empty_string_clears_to_default(void);
void test_bb_update_check_firmware_board_matches_named_asset(void);
void test_bb_update_check_firmware_board_default_does_not_match_named_asset(void);
void test_bb_update_check_firmware_board_with_bin_suffix_no_match(void);
void test_bb_update_check_firmware_board_custom_parser_receives_board(void);
void test_bb_update_check_init_alone_does_not_publish(void);
void test_bb_update_check_publish_initial_before_init_returns_invalid_state(void);
void test_bb_update_check_publish_initial_populates_ring(void);
void test_bb_update_check_publish_initial_snapshot_available_is_false(void);
void test_bb_update_check_get_status_returns_copy_of_cached_state(void);
void test_bb_update_check_get_status_reflects_failure(void);
void test_bb_update_check_kick_returns_ok_on_host(void);
void test_bb_update_check_status_enabled_is_true_by_default(void);
void test_bb_update_check_run_one_disabled_returns_ok_without_fetch(void);
void test_bb_update_check_status_enabled_reflects_nv_flag(void);
void test_bb_update_check_reenabled_runs_check(void);

void test_bb_event_routes_init_idempotent(void);
void test_bb_event_routes_init_null_cfg_uses_defaults(void);
void test_bb_event_routes_init_zero_cfg_fields_use_defaults(void);
void test_bb_event_routes_drain_null_buf_returns_zero(void);
void test_bb_event_routes_attach_returns_not_found_for_unregistered_topic(void);
void test_bb_event_routes_attach_dedupes_same_topic(void);
void test_bb_event_routes_attach_null_returns_invalid_arg(void);
void test_bb_event_routes_attach_before_init_returns_invalid_state(void);
void test_bb_event_routes_client_acquire_release_round_trip(void);
void test_bb_event_routes_client_acquire_null_out_returns_invalid_arg(void);
void test_bb_event_routes_client_acquire_before_init_returns_invalid_state(void);
void test_bb_event_routes_client_release_null_noop(void);
void test_bb_event_routes_client_pool_exhaustion_returns_no_space(void);
void test_bb_event_routes_drain_emits_sse_frame(void);
void test_bb_event_routes_drain_empty_payload_emits_object(void);
void test_bb_event_routes_drain_empty_queue_returns_zero(void);
void test_bb_event_routes_drain_null_client_returns_zero(void);
void test_bb_event_routes_drain_tiny_buf_returns_zero(void);
void test_bb_event_routes_two_clients_both_receive(void);
void test_bb_event_routes_queue_overflow_drops_oldest(void);
void test_bb_event_routes_client_acquire_replays_buffered_events(void);
void test_bb_event_routes_init_max_clients_above_cap_returns_invalid_arg(void);
void test_bb_event_routes_attach_table_full_returns_no_space(void);
void test_bb_event_routes_heartbeat_ms_returns_configured_value(void);
void test_bb_event_routes_reset_releases_held_client(void);
void test_bb_event_routes_capture_walks_past_non_matching_topic(void);
void test_bb_event_routes_drain_truncated_falls_back_to_safe_frame(void);
void test_bb_event_routes_client_acquire_entries_calloc_fails(void);
void test_bb_event_routes_client_acquire_payload_calloc_fails(void);
void test_bb_event_routes_attach_ring_allocation_fails(void);
void test_bb_event_routes_client_acquire_subscribe_failure_rolls_back(void);
void test_bb_event_routes_attach_ex_retained_true(void);
void test_bb_event_routes_client_acquire_ex_filters_to_matching_topic(void);
void test_bb_event_routes_client_acquire_ex_null_filter_subscribes_to_all(void);

// Forward declarations from test_bb_event_routes_diag.c
void test_bb_event_routes_topic_count_zero_before_attach(void);
void test_bb_event_routes_topic_count_increments_on_attach(void);
void test_bb_event_routes_topic_count_unchanged_on_dedup_attach(void);
void test_bb_event_routes_topic_info_out_of_range_returns_not_found(void);
void test_bb_event_routes_topic_info_returns_correct_name(void);
void test_bb_event_routes_topic_info_null_out_params_ok(void);
void test_bb_event_routes_topic_info_multiple_topics(void);
void test_bb_event_routes_topic_info_ring_reflects_posts(void);
void test_bb_event_routes_active_client_count_zero_before_acquire(void);
void test_bb_event_routes_active_client_count_increments_on_acquire(void);
void test_bb_event_routes_active_client_count_at_max(void);
void test_bb_event_routes_diag_full_round_trip(void);
void test_bb_event_routes_topic_count_zero_after_reset(void);

// Forward declarations from test_bb_led_anim.c
void bb_led_anim_test_reset(void);
void test_anim_attach_null_cfg_returns_invalid_arg(void);
void test_anim_attach_null_out_returns_invalid_arg(void);
void test_anim_detach_no_crash(void);
void test_anim_set_solid_on_onoff_handle_returns_ok(void);
void test_anim_set_rgb_pattern_on_onoff_handle_returns_unsupported(void);
void test_anim_set_chase_on_single_led_returns_unsupported(void);
void test_anim_blink_on_at_quarter_period(void);
void test_anim_blink_off_at_three_quarter_period(void);
void test_anim_breathe_brightness_rises_then_falls(void);
void test_anim_color_cycle_red_dominant_at_hue_zero(void);
void test_anim_color_cycle_green_dominant_at_one_third_period(void);
void test_anim_color_cycle_blue_dominant_at_two_thirds_period(void);
void test_anim_detach_null_returns_invalid_arg(void);

void setUp(void) {
    _bb_log_registry_reset();
    bb_mdns_host_reset();
    wifi_reconn_policy_test_reset();
    bb_mdns_lifecycle_test_reset();
    bb_led_test_reset();
    bb_led_pwm_test_reset();
    bb_led_apa102_host_test_reset();
    bb_led_anim_test_reset();
    bb_http_client_clear_mock();
    bb_update_check_reset_for_test();
    bb_event_routes_reset_for_test();
    bb_event_routes_reset_allocator();
    bb_event_reset_for_test();
    bb_event_port_reset_for_test();
    bb_event_ring_reset_allocator();
    bb_event_port_set_malloc(NULL);
    test_alloc_reset();
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

    // Panic log tests
    RUN_TEST(test_bb_diag_panic_available_returns_false_on_host);
    RUN_TEST(test_bb_diag_panic_get_returns_not_found_on_host);
    RUN_TEST(test_bb_diag_panic_get_invalid_args);
    RUN_TEST(test_bb_diag_panic_clear_is_safe_on_host);
    RUN_TEST(test_bb_diag_panic_clear_after_unavailable);
    RUN_TEST(test_bb_diag_abnormal_reset_count_returns_zero_on_host);
    RUN_TEST(test_bb_diag_abnormal_reset_count_clear_is_safe_on_host);

    // OTA pull tests
    RUN_TEST(test_ota_pull_skip_check_callback_registration);
    RUN_TEST(test_ota_pull_skip_check_callback_returns_true);
    RUN_TEST(test_ota_pull_skip_check_callback_returns_false);
    RUN_TEST(test_bb_ota_pull_set_http_timeout_ms_default_is_20000);
    RUN_TEST(test_bb_ota_pull_set_http_timeout_ms_zero_restores_default);

    // OTA pull — streaming manifest fetch
    RUN_TEST(test_ota_pull_manifest_fetch_success);
    RUN_TEST(test_ota_pull_manifest_transport_failure);
    RUN_TEST(test_ota_pull_manifest_http_404);
    RUN_TEST(test_ota_pull_manifest_board_asset_not_found);
    RUN_TEST(test_ota_pull_manifest_bad_json);
    RUN_TEST(test_ota_pull_manifest_realistic_github_payload);
    RUN_TEST(test_ota_pull_manifest_fallback_board_name);
    RUN_TEST(test_ota_pull_manifest_asset_found_ota_proceeds);

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

    // Release manifest tests
    RUN_TEST(test_bb_release_manifest_parse_github_valid_manifest);
    RUN_TEST(test_bb_release_manifest_parse_github_missing_tag);
    RUN_TEST(test_bb_release_manifest_parse_github_missing_asset);
    RUN_TEST(test_bb_release_manifest_parse_github_null_args);
    RUN_TEST(test_bb_release_manifest_parse_github_bad_json);
    RUN_TEST(test_bb_release_manifest_parse_github_multiple_assets);
    RUN_TEST(test_bb_release_manifest_parse_github_whitespace_around_colons);
    RUN_TEST(test_bb_release_manifest_parse_github_handles_escape_sequences);
    RUN_TEST(test_bb_release_manifest_parse_github_unicode_escape_skipped);
    RUN_TEST(test_bb_release_manifest_parse_github_skips_non_matching_first_asset);
    RUN_TEST(test_bb_release_manifest_parse_github_asset_missing_name_skipped);
    RUN_TEST(test_bb_release_manifest_parse_github_matching_asset_no_url);
    RUN_TEST(test_bb_release_manifest_parse_github_assets_not_array);
    RUN_TEST(test_bb_release_manifest_parse_github_empty_assets_array);

    // Streaming release manifest parser tests
    RUN_TEST(test_stream_begin_null_ctx_returns_invalid_arg);
    RUN_TEST(test_stream_begin_null_board_returns_invalid_arg);
    RUN_TEST(test_stream_begin_null_tag_returns_invalid_arg);
    RUN_TEST(test_stream_begin_null_url_returns_invalid_arg);
    RUN_TEST(test_stream_begin_zero_tag_cap_returns_invalid_arg);
    RUN_TEST(test_stream_begin_zero_url_cap_returns_invalid_arg);
    RUN_TEST(test_stream_begin_cap_of_one_returns_invalid_arg);
    RUN_TEST(test_stream_end_null_ctx_returns_invalid_arg);
    RUN_TEST(test_stream_whole_body_chunk);
    RUN_TEST(test_stream_256_byte_chunks);
    RUN_TEST(test_stream_7_byte_chunks);
    RUN_TEST(test_stream_1_byte_chunks);
    RUN_TEST(test_stream_skips_non_matching_first_asset);
    RUN_TEST(test_stream_skips_non_matching_first_asset_256);
    RUN_TEST(test_stream_missing_tag_returns_not_found);
    RUN_TEST(test_stream_missing_assets_returns_not_found);
    RUN_TEST(test_stream_no_matching_asset_returns_not_found);
    RUN_TEST(test_stream_empty_assets_array_returns_not_found);
    RUN_TEST(test_stream_bad_json_returns_not_found);
    RUN_TEST(test_stream_asset_missing_url_returns_not_found);
    RUN_TEST(test_stream_taipanminer_board_name);
    RUN_TEST(test_stream_backslash_slash_in_url);
    RUN_TEST(test_stream_feed_after_error_is_noop);
    RUN_TEST(test_stream_whitespace_around_colons_1byte);
    RUN_TEST(test_stream_backslash_backslash_in_url);
    RUN_TEST(test_stream_unicode_escape_in_url_dropped);
    RUN_TEST(test_stream_escape_in_asset_name_no_match);
    RUN_TEST(test_stream_board_name_truncation);
    RUN_TEST(test_stream_skip_string_with_escape);
    RUN_TEST(test_stream_toplevel_nested_array_skipped);
    RUN_TEST(test_stream_toplevel_nested_object_skipped);
    RUN_TEST(test_stream_skip_depth_string_with_braces);
    RUN_TEST(test_stream_skip_depth_string_with_escape);
    RUN_TEST(test_stream_escaped_key_char_ignored);
    RUN_TEST(test_stream_assets_array_exit_via_scan_key);
    RUN_TEST(test_stream_asset_scalar_field_skipped);
    RUN_TEST(test_stream_toplevel_scalar_field_skipped);
    RUN_TEST(test_stream_asset_with_nested_array_field_skipped);
    RUN_TEST(test_stream_asset_with_nested_object_field_skipped);
    RUN_TEST(test_stream_asset_name_at_capacity_truncated_no_match);
    RUN_TEST(test_stream_very_long_key_truncated_ignored);
    RUN_TEST(test_stream_long_key_with_escape_at_capacity_ignored);
    RUN_TEST(test_stream_skip_depth_nested_array_inside_object);
    RUN_TEST(test_stream_skip_depth_multi_level_nesting);

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
    RUN_TEST(test_bb_url_parse_bool_one);
    RUN_TEST(test_bb_url_parse_bool_zero);
    RUN_TEST(test_bb_url_parse_bool_true_lowercase);
    RUN_TEST(test_bb_url_parse_bool_true_uppercase);
    RUN_TEST(test_bb_url_parse_bool_false_lowercase);
    RUN_TEST(test_bb_url_parse_bool_on);
    RUN_TEST(test_bb_url_parse_bool_off);
    RUN_TEST(test_bb_url_parse_bool_yes);
    RUN_TEST(test_bb_url_parse_bool_no);
    RUN_TEST(test_bb_url_parse_bool_t);
    RUN_TEST(test_bb_url_parse_bool_f);
    RUN_TEST(test_bb_url_parse_bool_y);
    RUN_TEST(test_bb_url_parse_bool_n);
    RUN_TEST(test_bb_url_parse_bool_empty_string);
    RUN_TEST(test_bb_url_parse_bool_invalid_maybe);
    RUN_TEST(test_bb_url_parse_bool_invalid_two);
    RUN_TEST(test_bb_url_parse_bool_null_pointer);
    RUN_TEST(test_bb_url_parse_bool_prefix_of_valid);
    RUN_TEST(test_bb_url_parse_bool_extension_of_valid);
    RUN_TEST(test_bb_url_parse_uint_zero);
    RUN_TEST(test_bb_url_parse_uint_decimal_42);
    RUN_TEST(test_bb_url_parse_uint_max_unsigned_long);
    RUN_TEST(test_bb_url_parse_uint_empty_string);
    RUN_TEST(test_bb_url_parse_uint_non_digit);
    RUN_TEST(test_bb_url_parse_uint_trailing_junk);
    RUN_TEST(test_bb_url_parse_uint_leading_space);
    RUN_TEST(test_bb_url_parse_uint_negative_sign);
    RUN_TEST(test_bb_url_parse_uint_decimal_point);
    RUN_TEST(test_bb_url_parse_uint_null_pointer);
    RUN_TEST(test_bb_url_parse_uint_overflow);
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

    // bb_http JSON array streaming tests
    RUN_TEST(test_json_arr_begin_null_req);
    RUN_TEST(test_json_arr_begin_null_stream);
    RUN_TEST(test_json_arr_begin_init);
    RUN_TEST(test_json_arr_emit_unopened);
    RUN_TEST(test_json_arr_emit_after_end);
    RUN_TEST(test_json_arr_sticky_error);
    RUN_TEST(test_json_arr_empty);
    RUN_TEST(test_json_arr_single_item);
    RUN_TEST(test_json_arr_multiple_items);
    RUN_TEST(test_json_arr_emit_null_item);
    RUN_TEST(test_json_arr_emit_null_stream);
    RUN_TEST(test_json_arr_end_null_stream);

    // NV config tests
    RUN_TEST(test_nv_config_init_success);
    RUN_TEST(test_nv_config_wifi_ssid_empty_after_init);
    RUN_TEST(test_nv_config_wifi_pass_empty_after_init);
    RUN_TEST(test_nv_config_display_enabled_default_true);
    RUN_TEST(test_nv_config_is_provisioned_stub_returns_false);
    RUN_TEST(test_hostname_default_empty);
    RUN_TEST(test_hostname_set_get_roundtrip);
    RUN_TEST(test_hostname_validates_charset);
    RUN_TEST(test_hostname_validates_length);
    RUN_TEST(test_hostname_rejects_leading_hyphen);
    RUN_TEST(test_hostname_rejects_trailing_hyphen);
    RUN_TEST(test_hostname_rejects_null);

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
    RUN_TEST(test_nv_batch_begin_null_batch);
    RUN_TEST(test_nv_batch_begin_null_ns);
    RUN_TEST(test_nv_batch_begin_valid);
    RUN_TEST(test_nv_batch_set_u32_before_begin);
    RUN_TEST(test_nv_batch_set_u32_null_key);
    RUN_TEST(test_nv_batch_set_u32_valid);
    RUN_TEST(test_nv_batch_set_str_null_value);
    RUN_TEST(test_nv_batch_commit_null);
    RUN_TEST(test_nv_batch_set_after_commit_rejected);
    RUN_TEST(test_nv_batch_three_u32_writes_succeed);

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
    RUN_TEST(test_route_schemas_manifest_fixture_parses);
    RUN_TEST(test_route_schemas_registry_all_valid);
    RUN_TEST(test_route_schemas_walker_flags_malformed);
    RUN_TEST(test_set_raw_writes_null_on_malformed_json);
    RUN_TEST(test_set_raw_writes_parsed_object_on_valid_json);

    // Capture harness unit tests
    RUN_TEST(test_capture_begin_end_basic);
    RUN_TEST(test_capture_default_status_is_200);
    RUN_TEST(test_capture_send_json_sets_content_type);
    RUN_TEST(test_capture_multi_write_appends);
    RUN_TEST(test_capture_end_null_args_returns_err);
    RUN_TEST(test_capture_no_active_slot_ignored);

    // Fidelity audit: handler output vs declared response schema
    RUN_TEST(test_fidelity_reboot);
    RUN_TEST(test_fidelity_board);
    RUN_TEST(test_fidelity_info);
    RUN_TEST(test_fidelity_health);
    RUN_TEST(test_fidelity_wifi_info);
    RUN_TEST(test_fidelity_ota_status);
    RUN_TEST(test_fidelity_ota_mark_valid_409);
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
    RUN_TEST(test_openapi_emit_parameters_array_present);
    RUN_TEST(test_openapi_emit_parameters_absent_when_null);
    RUN_TEST(test_openapi_emit_param_null_description_omitted);
    RUN_TEST(test_openapi_emit_param_in_path);
    RUN_TEST(test_openapi_emit_multiple_params_on_route);
    RUN_TEST(test_openapi_emit_param_count_zero_omits_parameters);
    RUN_TEST(test_openapi_emit_param_null_name_defaults_to_empty);
    RUN_TEST(test_openapi_emit_param_null_in_defaults_to_query);
    RUN_TEST(test_openapi_emit_oom_params_arr_skips_parameters);
    RUN_TEST(test_openapi_emit_oom_param_obj_skips_entry);
    RUN_TEST(test_openapi_emit_oom_schema_obj_skips_schema);

    // bb_openapi_validate tests
    RUN_TEST(test_validate_null_schema_json_returns_invalid_arg);
    RUN_TEST(test_validate_null_value_returns_invalid_arg);
    RUN_TEST(test_validate_malformed_schema_returns_invalid_arg);
    RUN_TEST(test_validate_type_string_match);
    RUN_TEST(test_validate_type_string_mismatch);
    RUN_TEST(test_validate_type_integer_match);
    RUN_TEST(test_validate_type_integer_mismatch);
    RUN_TEST(test_validate_type_boolean_match);
    RUN_TEST(test_validate_type_boolean_mismatch);
    RUN_TEST(test_validate_type_object_match);
    RUN_TEST(test_validate_type_object_mismatch);
    RUN_TEST(test_validate_type_array_match);
    RUN_TEST(test_validate_type_array_mismatch);
    RUN_TEST(test_validate_err_null_still_returns_validation_code);
    RUN_TEST(test_validate_required_present);
    RUN_TEST(test_validate_required_missing);
    RUN_TEST(test_validate_properties_nested_ok);
    RUN_TEST(test_validate_properties_nested_type_mismatch);
    RUN_TEST(test_validate_properties_deeply_nested);
    RUN_TEST(test_validate_items_all_ok);
    RUN_TEST(test_validate_items_element_mismatch);
    RUN_TEST(test_validate_items_nested_object_element);
    RUN_TEST(test_validate_enum_match);
    RUN_TEST(test_validate_enum_mismatch);
    RUN_TEST(test_validate_additional_properties_false_ok);
    RUN_TEST(test_validate_additional_properties_false_rejects_extra);
    RUN_TEST(test_validate_additional_properties_default_allows_extra);
    RUN_TEST(test_validate_unknown_keyword_does_not_fail);
    RUN_TEST(test_validate_unknown_type_value_passes);
    RUN_TEST(test_validate_enum_numeric_mismatch_renders_value);
    RUN_TEST(test_validate_smoke_reboot_schema);
    RUN_TEST(test_validate_smoke_ota_check_schema);
    RUN_TEST(test_validate_smoke_log_level_schema);
    RUN_TEST(test_validate_smoke_panic_schema);
    RUN_TEST(test_validate_enum_mismatch_null_err);
    RUN_TEST(test_validate_required_missing_null_err);
    RUN_TEST(test_validate_additional_properties_rejection_null_err);
    RUN_TEST(test_validate_malformed_required_non_array_ignored);
    RUN_TEST(test_validate_required_non_string_item_skipped);
    RUN_TEST(test_validate_malformed_properties_non_object_ignored);
    RUN_TEST(test_validate_malformed_type_non_string_ignored);
    RUN_TEST(test_validate_malformed_enum_non_array_ignored);
    RUN_TEST(test_validate_required_non_object_value_skipped);
    RUN_TEST(test_validate_properties_non_object_value_skipped);
    RUN_TEST(test_validate_additional_properties_true_allows_extra);
    RUN_TEST(test_validate_items_non_array_value_skipped);
    RUN_TEST(test_validate_enum_empty_always_fails);
    RUN_TEST(test_validate_enum_bool_value_renders_non_string);
    RUN_TEST(test_validate_enum_null_value_renders_non_string);
    RUN_TEST(test_validate_schema_not_object_passes);
    RUN_TEST(test_validate_schema_array_not_object_passes);
    RUN_TEST(test_validate_deep_path_object_array_object);
    RUN_TEST(test_validate_path_stack_overflow_does_not_crash);
    RUN_TEST(test_validate_type_number_match);
    RUN_TEST(test_validate_type_number_mismatch);
    RUN_TEST(test_validate_type_null_match);
    RUN_TEST(test_validate_type_null_mismatch);
    RUN_TEST(test_validate_path_render_truncation);
    RUN_TEST(test_validate_path_render_buffer_full_midloop);
    RUN_TEST(test_validate_path_push_index_overflow);
    RUN_TEST(test_validate_additional_properties_non_bool_ignored);

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
    RUN_TEST(test_bb_registry_init_honors_order_priority);
    RUN_TEST(test_bb_registry_init_same_order_preserves_insertion_order);
    RUN_TEST(test_bb_registry_init_order_mixed);
    RUN_TEST(test_bb_registry_pre_http_starts_empty);
    RUN_TEST(test_bb_registry_pre_http_add_increments_count);
    RUN_TEST(test_bb_registry_pre_http_foreach_visits_in_insertion_order);
    RUN_TEST(test_bb_registry_pre_http_init_calls_each_fn);
    RUN_TEST(test_bb_registry_pre_http_init_reports_first_error_but_continues);
    RUN_TEST(test_bb_registry_pre_http_clear_resets_count);

    // bb_byte_order tests
    RUN_TEST(test_bb_load_be32_constant);
    RUN_TEST(test_bb_load_le32_constant);
    RUN_TEST(test_bb_store_be32_round_trip);
    RUN_TEST(test_bb_store_le32_round_trip);
    RUN_TEST(test_bb_load_be32_store_be32_round_trip);
    RUN_TEST(test_bb_load_le32_store_le32_round_trip);
    RUN_TEST(test_bb_load_be16_constant);
    RUN_TEST(test_bb_load_le16_constant);
    RUN_TEST(test_bb_load_be16_store_be16_round_trip);
    RUN_TEST(test_bb_load_le16_store_le16_round_trip);
    RUN_TEST(test_bb_load_be32_misaligned);
    RUN_TEST(test_bb_store_be32_misaligned);
    RUN_TEST(test_bb_load_be16_misaligned);
    RUN_TEST(test_bb_store_be16_misaligned);

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

    // bb_led tests
    RUN_TEST(test_bb_led_caps_and_count);
    RUN_TEST(test_bb_led_set_on);
    RUN_TEST(test_bb_led_set_color_unsupported_when_no_rgb_cap);
    RUN_TEST(test_bb_led_idx_out_of_range);
    RUN_TEST(test_bb_led_fill_color_iterates);
    RUN_TEST(test_bb_led_close_calls_driver);
    RUN_TEST(test_bb_led_brightness_pct_validation);

    // bb_led_gpio tests
    RUN_TEST(test_gpio_open_close);
    RUN_TEST(test_gpio_active_high_set_on);
    RUN_TEST(test_gpio_active_low_set_on);
    RUN_TEST(test_gpio_idx_must_be_zero);
    RUN_TEST(test_gpio_initial_state_off);
    RUN_TEST(test_gpio_initial_state_off_active_low);
    RUN_TEST(test_gpio_invalid_args);

    // bb_led_pwm tests
    RUN_TEST(test_pwm_open_close);
    RUN_TEST(test_pwm_set_brightness_active_high);
    RUN_TEST(test_pwm_set_brightness_active_low);
    RUN_TEST(test_pwm_set_on_active_high);
    RUN_TEST(test_pwm_set_on_active_low);
    RUN_TEST(test_pwm_set_color_unsupported);
    RUN_TEST(test_pwm_idx_must_be_zero);
    RUN_TEST(test_pwm_invalid_args);
    RUN_TEST(test_pwm_initial_state_off_active_high);
    RUN_TEST(test_pwm_initial_state_off_active_low);

    // bb_led_apa102 tests
    RUN_TEST(test_apa102_open_close);
    RUN_TEST(test_apa102_initial_flush_dark);
    RUN_TEST(test_apa102_set_color_and_flush);
    RUN_TEST(test_apa102_set_brightness_partial);
    RUN_TEST(test_apa102_fill_color);
    RUN_TEST(test_apa102_idx_out_of_range);
    RUN_TEST(test_apa102_invalid_args);
    RUN_TEST(test_apa102_disabled_pixel_zeros_rgb);

    // bb_button tests
    RUN_TEST(test_bb_button_open_null_cfg);
    RUN_TEST(test_bb_button_open_null_out);
    RUN_TEST(test_bb_button_press_past_debounce_fires_cb);
    RUN_TEST(test_bb_button_second_press_within_debounce_suppressed);
    RUN_TEST(test_bb_button_press_then_release_fires_two_cbs);
    RUN_TEST(test_bb_button_is_pressed_reflects_debounced_state);
    RUN_TEST(test_bb_button_get_queue_returns_null_on_host);
    RUN_TEST(test_bb_button_active_low_false_high_is_press);
    RUN_TEST(test_bb_button_close_subsequent_inject_noop);

    // bb_button_gpio tests
    RUN_TEST(test_btn_gpio_open_close);
    RUN_TEST(test_btn_gpio_initial_state_not_pressed);
    RUN_TEST(test_btn_gpio_inject_sets_pressed);
    RUN_TEST(test_btn_gpio_inject_sets_released);
    RUN_TEST(test_btn_gpio_invalid_args);
    RUN_TEST(test_btn_gpio_poll_noop_on_host);

    // bb_button_events tests
    RUN_TEST(test_btn_evt_attach_null_cfg_returns_invalid_arg);
    RUN_TEST(test_btn_evt_attach_null_out_returns_invalid_arg);
    RUN_TEST(test_btn_evt_attach_null_button_returns_invalid_arg);
    RUN_TEST(test_btn_evt_single_click_emits_exactly_one_click);
    RUN_TEST(test_btn_evt_double_click_emits_exactly_one_double_click);
    RUN_TEST(test_btn_evt_double_click_no_click_emitted);
    RUN_TEST(test_btn_evt_long_press_start_fires_once);
    RUN_TEST(test_btn_evt_repeat_events_monotonic_held_ms);
    RUN_TEST(test_btn_evt_long_press_end_correct_held_ms);
    RUN_TEST(test_btn_evt_no_repeat_after_long_press_end);
    RUN_TEST(test_btn_evt_medium_press_no_event);
    RUN_TEST(test_btn_evt_detach_no_crash_on_subsequent_events);

    // bb_event tests
    RUN_TEST(test_bb_event_init_topic_register_subscribe_post_pump_fires);
    RUN_TEST(test_bb_event_two_subscribers_both_receive);
    RUN_TEST(test_bb_event_unsubscribe_prevents_future_events);
    RUN_TEST(test_bb_event_post_exceeds_max_payload_returns_invalid_arg);
    RUN_TEST(test_bb_event_queue_overflow_returns_no_space);
    RUN_TEST(test_bb_event_topic_lookup_returns_same_handle);
    RUN_TEST(test_bb_event_topic_register_duplicate_returns_same_handle);
    RUN_TEST(test_bb_event_different_topics_dont_cross);
    RUN_TEST(test_bb_event_payload_integrity);
    RUN_TEST(test_bb_event_id_parameter_preserved);
    RUN_TEST(test_bb_event_init_null_cfg_uses_defaults);
    RUN_TEST(test_bb_event_init_idempotent);
    RUN_TEST(test_bb_event_topic_register_null_name_returns_invalid_arg);
    RUN_TEST(test_bb_event_topic_register_null_out_returns_invalid_arg);
    // RUN_TEST(test_bb_event_topic_register_returns_ok_when_initialized);
    // RUN_TEST(test_bb_event_topic_register_exceeds_max_returns_no_space);
    RUN_TEST(test_bb_event_topic_lookup_null_name_returns_invalid_arg);
    RUN_TEST(test_bb_event_topic_lookup_null_out_returns_invalid_arg);
    RUN_TEST(test_bb_event_topic_lookup_not_found);
    RUN_TEST(test_bb_event_subscribe_null_topic_returns_invalid_arg);
    RUN_TEST(test_bb_event_subscribe_null_callback_returns_invalid_arg);
    RUN_TEST(test_bb_event_subscribe_null_out_returns_invalid_arg);
    RUN_TEST(test_bb_event_unsubscribe_null_returns_invalid_arg);
    RUN_TEST(test_bb_event_post_null_topic_returns_invalid_arg);
    RUN_TEST(test_bb_event_post_payload_too_large_returns_invalid_arg);
    RUN_TEST(test_bb_event_post_with_small_payload_succeeds);
    RUN_TEST(test_bb_event_init_cfg_with_nonzero_values);
    RUN_TEST(test_bb_event_post_zero_payload_no_data);
    RUN_TEST(test_bb_event_unsubscribe_early_in_list);
    RUN_TEST(test_init_pool_guard_returns_early);
    RUN_TEST(test_init_with_zero_queue_depth_uses_default);
    RUN_TEST(test_init_with_zero_max_payload_uses_default);
    RUN_TEST(test_init_port_init_failure_returns_error);
    RUN_TEST(test_topic_register_before_init_returns_invalid_state);
    RUN_TEST(test_unsubscribe_non_head_subscriber);
    RUN_TEST(test_dispatch_null_entry_no_crash);
    RUN_TEST(test_bb_event_topic_register_walks_existing_entries);
    RUN_TEST(test_bb_event_topic_register_returns_no_space_when_full);
    RUN_TEST(test_bb_event_topic_lookup_walks_past_non_matches);
    RUN_TEST(test_bb_event_post_exceeds_max_payload_at_runtime_limit);

    // bb_event_ring tests
    RUN_TEST(test_bb_event_ring_attach_and_post_replay_delivers_all_entries);
    RUN_TEST(test_bb_event_ring_capacity_overflow_evicts_oldest);
    RUN_TEST(test_bb_event_ring_live_events_fire_after_subscribe);
    RUN_TEST(test_bb_event_ring_detach_stops_capturing);
    RUN_TEST(test_bb_event_ring_payload_integrity);
    RUN_TEST(test_bb_event_ring_attach_null_topic_returns_invalid_arg);
    RUN_TEST(test_bb_event_ring_attach_zero_capacity_returns_invalid_arg);
    RUN_TEST(test_bb_event_ring_attach_zero_max_entry_returns_invalid_arg);
    RUN_TEST(test_bb_event_ring_attach_null_out_returns_invalid_arg);
    RUN_TEST(test_bb_event_ring_subscribe_null_ring_returns_invalid_arg);
    RUN_TEST(test_bb_event_ring_subscribe_null_callback_returns_invalid_arg);
    RUN_TEST(test_bb_event_ring_subscribe_null_out_returns_invalid_arg);
    RUN_TEST(test_bb_event_ring_detach_null_noop);
    RUN_TEST(test_bb_event_ring_head_wraps_modulo_capacity);
    RUN_TEST(test_bb_event_ring_zero_payload_capture);
    RUN_TEST(test_bb_event_ring_empty_ring_replay);
    RUN_TEST(test_bb_event_ring_payload_with_data);
    RUN_TEST(test_ring_capture_with_size_nonzero_data_null);
    RUN_TEST(test_ring_attach_struct_calloc_fails);
    RUN_TEST(test_ring_attach_entries_calloc_fails);
    RUN_TEST(test_ring_attach_payload_calloc_fails);
    RUN_TEST(test_ring_subscribe_with_replay_snapshot_calloc_fails);
    RUN_TEST(test_ring_subscribe_when_subscriber_pool_exhausted);
    RUN_TEST(test_bb_event_ring_capture_null_data_with_size);
    RUN_TEST(test_bb_event_ring_attach_subscribe_failure_frees_all);
    RUN_TEST(test_bb_event_ring_subscribe_replay_second_alloc_failure_frees_first);

    // bb_event_ring retained-flag tests
    RUN_TEST(test_bb_event_ring_attach_ex_retained_true_returns_ok);
    RUN_TEST(test_bb_event_ring_attach_ex_retained_false_same_as_attach);
    RUN_TEST(test_bb_event_ring_retained_subscribe_after_one_post_replays);
    RUN_TEST(test_bb_event_ring_retained_capacity1_overflow_delivers_most_recent);
    RUN_TEST(test_bb_event_ring_retained_overflow_delivers_most_recent_n);
    RUN_TEST(test_bb_event_ring_attach_ex_null_topic_returns_invalid_arg);
    RUN_TEST(test_bb_event_ring_attach_ex_null_out_returns_invalid_arg);

    // bb_event_ring introspection tests
    RUN_TEST(test_bb_event_ring_capacity_null_returns_zero);
    RUN_TEST(test_bb_event_ring_capacity_returns_configured_value);
    RUN_TEST(test_bb_event_ring_count_null_returns_zero);
    RUN_TEST(test_bb_event_ring_count_empty_ring_returns_zero);
    RUN_TEST(test_bb_event_ring_count_after_posts);
    RUN_TEST(test_bb_event_ring_count_capped_at_capacity);
    RUN_TEST(test_bb_event_ring_last_entry_info_null_ring_returns_invalid_arg);
    RUN_TEST(test_bb_event_ring_last_entry_info_empty_ring_returns_not_found);
    RUN_TEST(test_bb_event_ring_last_entry_info_populated_ring);
    RUN_TEST(test_bb_event_ring_last_entry_info_reflects_latest_post);
    RUN_TEST(test_bb_event_ring_last_entry_info_after_eviction);
    RUN_TEST(test_bb_event_ring_last_entry_info_null_out_params_ok);
    RUN_TEST(test_bb_event_ring_last_entry_info_zero_size_payload);

    RUN_TEST(test_bb_event_subscribe_with_prep_runs_prep_before_subscribe);
    RUN_TEST(test_bb_event_subscribe_with_prep_null_prep_subscribes);
    RUN_TEST(test_bb_event_subscribe_with_prep_invalid_args);
    RUN_TEST(test_bb_event_lock_unlock_round_trip);

    // bb_event_routes tests
    // bb_http_client tests
    RUN_TEST(test_bb_http_client_get_null_url_returns_invalid_arg);
    RUN_TEST(test_bb_http_client_get_null_body_returns_invalid_arg);
    RUN_TEST(test_bb_http_client_get_null_out_returns_invalid_arg);
    RUN_TEST(test_bb_http_client_get_zero_cap_returns_invalid_arg);
    RUN_TEST(test_bb_http_client_get_no_mock_returns_invalid_state);
    RUN_TEST(test_bb_http_client_get_mock_success_returns_body);
    RUN_TEST(test_bb_http_client_get_mock_404_returns_ok_with_status);
    RUN_TEST(test_bb_http_client_get_mock_truncates_when_body_too_big);
    RUN_TEST(test_bb_http_client_get_mock_transport_error_returns_passthrough);
    RUN_TEST(test_bb_http_client_get_empty_body_is_valid);
    RUN_TEST(test_bb_http_client_get_cfg_honored);

    // bb_http_client_get_stream tests
    RUN_TEST(test_bb_http_client_stream_null_url_returns_invalid_arg);
    RUN_TEST(test_bb_http_client_stream_null_cb_returns_invalid_arg);
    RUN_TEST(test_bb_http_client_stream_null_out_returns_invalid_arg);
    RUN_TEST(test_bb_http_client_stream_transport_error_propagated);
    RUN_TEST(test_bb_http_client_stream_small_body_reassembled);
    RUN_TEST(test_bb_http_client_stream_large_body_multiple_chunks);
    RUN_TEST(test_bb_http_client_stream_empty_body);
    RUN_TEST(test_bb_http_client_stream_early_stop_sets_truncated);
    RUN_TEST(test_bb_http_client_stream_cb_error_propagated);
    RUN_TEST(test_bb_http_client_stream_404_status_code);
    RUN_TEST(test_bb_http_client_stream_cfg_honored);

    // bb_update_check tests
    RUN_TEST(test_bb_update_check_init_idempotent);
    RUN_TEST(test_bb_update_check_init_with_cfg_uses_overrides);
    RUN_TEST(test_bb_update_check_get_status_before_init_returns_invalid_state);
    RUN_TEST(test_bb_update_check_get_status_null_out_returns_invalid_arg);
    RUN_TEST(test_bb_update_check_set_releases_url_validates);
    RUN_TEST(test_bb_update_check_set_releases_url_before_init_returns_invalid_state);
    RUN_TEST(test_bb_update_check_set_parser_before_init_returns_invalid_state);
    RUN_TEST(test_bb_update_check_set_parser_null_restores_default);
    RUN_TEST(test_bb_update_check_run_one_before_init_returns_invalid_arg);
    RUN_TEST(test_bb_update_check_run_one_without_url_returns_invalid_state);
    RUN_TEST(test_bb_update_check_now_without_url_returns_invalid_state);
    RUN_TEST(test_bb_update_check_now_before_init_returns_invalid_arg);
    RUN_TEST(test_bb_update_check_run_one_newer_release_flips_available);
    RUN_TEST(test_bb_update_check_run_one_same_version_keeps_unavailable);
    RUN_TEST(test_bb_update_check_run_one_transport_failure_sticky);
    RUN_TEST(test_bb_update_check_run_one_http_404_sticky_failure);
    RUN_TEST(test_bb_update_check_run_one_parse_failure_sticky);
    RUN_TEST(test_bb_update_check_run_one_recovers_after_failure);
    RUN_TEST(test_bb_update_check_run_one_custom_parser_invoked);
    RUN_TEST(test_bb_update_check_now_drives_a_check);
    RUN_TEST(test_bb_update_check_post_initial_publishes_on_first_check);
    RUN_TEST(test_bb_update_check_dev_tag_treated_as_older);
    RUN_TEST(test_bb_update_check_run_one_newer_to_same_transitions_back);
    RUN_TEST(test_bb_update_check_custom_parser_transport_error);
    RUN_TEST(test_bb_update_check_custom_parser_parse_failure);
    RUN_TEST(test_bb_update_check_custom_parser_http_404);
    RUN_TEST(test_bb_update_check_custom_parser_body_exceeds_16k);
    RUN_TEST(test_bb_update_check_custom_parser_post_initial_publishes);
    RUN_TEST(test_bb_update_check_set_hooks_before_init_returns_invalid_state);
    RUN_TEST(test_bb_update_check_set_hooks_null_clears);
    RUN_TEST(test_bb_update_check_hooks_called_in_order_on_success);
    RUN_TEST(test_bb_update_check_hooks_resume_fires_on_transport_error);
    RUN_TEST(test_bb_update_check_hooks_resume_fires_on_parse_error);
    RUN_TEST(test_bb_update_check_hooks_called_once_per_run);
    RUN_TEST(test_bb_update_check_hooks_custom_parser_success);
    RUN_TEST(test_bb_update_check_hooks_custom_parser_transport_error);
    RUN_TEST(test_bb_update_check_hooks_custom_parser_parse_error);
    RUN_TEST(test_bb_update_check_pause_returns_false_skips_fetch);
    RUN_TEST(test_bb_update_check_pause_returns_false_custom_parser_skips_fetch);
    RUN_TEST(test_bb_update_check_set_firmware_board_before_init_returns_invalid_state);
    RUN_TEST(test_bb_update_check_set_firmware_board_too_long_returns_invalid_arg);
    RUN_TEST(test_bb_update_check_set_firmware_board_null_clears_to_default);
    RUN_TEST(test_bb_update_check_set_firmware_board_empty_string_clears_to_default);
    RUN_TEST(test_bb_update_check_firmware_board_matches_named_asset);
    RUN_TEST(test_bb_update_check_firmware_board_default_does_not_match_named_asset);
    RUN_TEST(test_bb_update_check_firmware_board_with_bin_suffix_no_match);
    RUN_TEST(test_bb_update_check_firmware_board_custom_parser_receives_board);
    RUN_TEST(test_bb_update_check_init_alone_does_not_publish);
    RUN_TEST(test_bb_update_check_publish_initial_before_init_returns_invalid_state);
    RUN_TEST(test_bb_update_check_publish_initial_populates_ring);
    RUN_TEST(test_bb_update_check_publish_initial_snapshot_available_is_false);
    RUN_TEST(test_bb_update_check_get_status_returns_copy_of_cached_state);
    RUN_TEST(test_bb_update_check_get_status_reflects_failure);
    RUN_TEST(test_bb_update_check_kick_returns_ok_on_host);
    RUN_TEST(test_bb_update_check_status_enabled_is_true_by_default);
    RUN_TEST(test_bb_update_check_run_one_disabled_returns_ok_without_fetch);
    RUN_TEST(test_bb_update_check_status_enabled_reflects_nv_flag);
    RUN_TEST(test_bb_update_check_reenabled_runs_check);

    RUN_TEST(test_bb_event_routes_init_idempotent);
    RUN_TEST(test_bb_event_routes_init_null_cfg_uses_defaults);
    RUN_TEST(test_bb_event_routes_init_zero_cfg_fields_use_defaults);
    RUN_TEST(test_bb_event_routes_drain_null_buf_returns_zero);
    RUN_TEST(test_bb_event_routes_attach_returns_not_found_for_unregistered_topic);
    RUN_TEST(test_bb_event_routes_attach_dedupes_same_topic);
    RUN_TEST(test_bb_event_routes_attach_null_returns_invalid_arg);
    RUN_TEST(test_bb_event_routes_attach_before_init_returns_invalid_state);
    RUN_TEST(test_bb_event_routes_client_acquire_release_round_trip);
    RUN_TEST(test_bb_event_routes_client_acquire_null_out_returns_invalid_arg);
    RUN_TEST(test_bb_event_routes_client_acquire_before_init_returns_invalid_state);
    RUN_TEST(test_bb_event_routes_client_release_null_noop);
    RUN_TEST(test_bb_event_routes_client_pool_exhaustion_returns_no_space);
    RUN_TEST(test_bb_event_routes_drain_emits_sse_frame);
    RUN_TEST(test_bb_event_routes_drain_empty_payload_emits_object);
    RUN_TEST(test_bb_event_routes_drain_empty_queue_returns_zero);
    RUN_TEST(test_bb_event_routes_drain_null_client_returns_zero);
    RUN_TEST(test_bb_event_routes_drain_tiny_buf_returns_zero);
    RUN_TEST(test_bb_event_routes_two_clients_both_receive);
    RUN_TEST(test_bb_event_routes_queue_overflow_drops_oldest);
    RUN_TEST(test_bb_event_routes_client_acquire_replays_buffered_events);
    RUN_TEST(test_bb_event_routes_init_max_clients_above_cap_returns_invalid_arg);
    RUN_TEST(test_bb_event_routes_attach_table_full_returns_no_space);
    RUN_TEST(test_bb_event_routes_heartbeat_ms_returns_configured_value);
    RUN_TEST(test_bb_event_routes_reset_releases_held_client);
    RUN_TEST(test_bb_event_routes_capture_walks_past_non_matching_topic);
    RUN_TEST(test_bb_event_routes_drain_truncated_falls_back_to_safe_frame);
    RUN_TEST(test_bb_event_routes_client_acquire_entries_calloc_fails);
    RUN_TEST(test_bb_event_routes_client_acquire_payload_calloc_fails);
    RUN_TEST(test_bb_event_routes_attach_ring_allocation_fails);
    RUN_TEST(test_bb_event_routes_client_acquire_subscribe_failure_rolls_back);
    RUN_TEST(test_bb_event_routes_attach_ex_retained_true);
    RUN_TEST(test_bb_event_routes_client_acquire_ex_filters_to_matching_topic);
    RUN_TEST(test_bb_event_routes_client_acquire_ex_null_filter_subscribes_to_all);

    // bb_event_routes diag tests
    RUN_TEST(test_bb_event_routes_topic_count_zero_before_attach);
    RUN_TEST(test_bb_event_routes_topic_count_increments_on_attach);
    RUN_TEST(test_bb_event_routes_topic_count_unchanged_on_dedup_attach);
    RUN_TEST(test_bb_event_routes_topic_info_out_of_range_returns_not_found);
    RUN_TEST(test_bb_event_routes_topic_info_returns_correct_name);
    RUN_TEST(test_bb_event_routes_topic_info_null_out_params_ok);
    RUN_TEST(test_bb_event_routes_topic_info_multiple_topics);
    RUN_TEST(test_bb_event_routes_topic_info_ring_reflects_posts);
    RUN_TEST(test_bb_event_routes_active_client_count_zero_before_acquire);
    RUN_TEST(test_bb_event_routes_active_client_count_increments_on_acquire);
    RUN_TEST(test_bb_event_routes_active_client_count_at_max);
    RUN_TEST(test_bb_event_routes_diag_full_round_trip);
    RUN_TEST(test_bb_event_routes_topic_count_zero_after_reset);

    // bb_led_anim tests
    RUN_TEST(test_anim_attach_null_cfg_returns_invalid_arg);
    RUN_TEST(test_anim_attach_null_out_returns_invalid_arg);
    RUN_TEST(test_anim_detach_no_crash);
    RUN_TEST(test_anim_set_solid_on_onoff_handle_returns_ok);
    RUN_TEST(test_anim_set_rgb_pattern_on_onoff_handle_returns_unsupported);
    RUN_TEST(test_anim_set_chase_on_single_led_returns_unsupported);
    RUN_TEST(test_anim_blink_on_at_quarter_period);
    RUN_TEST(test_anim_blink_off_at_three_quarter_period);
    RUN_TEST(test_anim_breathe_brightness_rises_then_falls);
    RUN_TEST(test_anim_color_cycle_red_dominant_at_hue_zero);
    RUN_TEST(test_anim_color_cycle_green_dominant_at_one_third_period);
    RUN_TEST(test_anim_color_cycle_blue_dominant_at_two_thirds_period);
    RUN_TEST(test_anim_detach_null_returns_invalid_arg);

    return UNITY_END();
}
