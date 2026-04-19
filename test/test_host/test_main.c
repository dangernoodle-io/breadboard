#include "unity.h"

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

// Forward declarations from test_nv_generic.c
void test_nv_set_u8_null_ns(void);
void test_nv_set_u8_null_key(void);
void test_nv_set_u8_valid(void);
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

void setUp(void) {}
void tearDown(void) {}

int main(void) {
    UNITY_BEGIN();

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

    return UNITY_END();
}
