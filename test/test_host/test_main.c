#include "unity.h"
#include "../../components/bb_log/src/bb_log_internal.h"

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

void setUp(void) {
    _bb_log_registry_reset();
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

    // bb_wifi tests
    RUN_TEST(test_bb_wifi_set_hostname_null);
    RUN_TEST(test_bb_wifi_set_hostname_empty);
    RUN_TEST(test_bb_wifi_set_hostname_valid);

    return UNITY_END();
}
