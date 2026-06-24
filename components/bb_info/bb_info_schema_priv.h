#pragma once

// Private: shared between platform/espidf and platform/host bb_info implementations.
//
// bb_section_assemble_schema(reg, k_info_schema_base, k_info_schema_suffix) builds the
// complete /api/info 200 JSON-Schema:
//   k_info_schema_base + [,"<name>":<schema_props>]* + k_info_schema_suffix
//
// Root-level fields (board, network, capabilities, http_handler_*) live in base/suffix.
// Named sections (display, led, ntp, diag) are emitted by bb_section_assemble_schema
// from each section's schema_props string.
//
// NOTE: abnormal_reset_count and panic have been REMOVED from root.
//       They live in the diag section: diag.{wdt_resets, panic?}.

static const char k_info_schema_base[] =
    "{\"type\":\"object\","
    "\"properties\":{"
    "\"board\":{\"type\":\"string\"},"
    "\"project_name\":{\"type\":\"string\"},"
    "\"version\":{\"type\":\"string\"},"
    "\"idf_version\":{\"type\":\"string\"},"
    "\"build_date\":{\"type\":\"string\"},"
    "\"build_time\":{\"type\":\"string\"},"
    "\"chip_model\":{\"type\":\"string\"},"
    "\"cores\":{\"type\":\"integer\"},"
    "\"mac\":{\"type\":\"string\"},"
    "\"flash_size\":{\"type\":\"integer\"},"
    "\"app_size\":{\"type\":\"integer\"},"
    "\"reset_reason\":{\"type\":\"string\"},"
    "\"ota_validated\":{\"type\":\"boolean\"},"
    "\"ota_ready\":{\"type\":\"boolean\"},"
    "\"chip_revision\":{\"type\":\"integer\"},"
    "\"cpu_freq_mhz\":{\"type\":\"integer\"},"
    "\"heap_internal\":{\"type\":\"object\","
    "\"properties\":{"
    "\"free\":{\"type\":\"integer\"},"
    "\"total\":{\"type\":\"integer\"},"
    "\"min_free\":{\"type\":\"integer\"},"
    "\"largest_block\":{\"type\":\"integer\"}}},"
    "\"heap_psram\":{\"type\":\"object\","
    "\"properties\":{"
    "\"free\":{\"type\":\"integer\"},"
    "\"total\":{\"type\":\"integer\"}}},"
    "\"rtc\":{\"type\":\"object\","
    "\"properties\":{"
    "\"used\":{\"type\":\"integer\"},"
    "\"total\":{\"type\":\"integer\"}}},"
    "\"static_dram\":{\"type\":\"object\","
    "\"properties\":{"
    "\"bytes\":{\"type\":\"integer\"}}},"
    "\"network\":{\"type\":\"object\","
    "\"properties\":{"
    "\"ssid\":{\"type\":\"string\"},"
    "\"bssid\":{\"type\":\"string\"},"
    "\"rssi\":{\"type\":\"integer\"},"
    "\"ip\":{\"type\":\"string\"},"
    "\"connected\":{\"type\":\"boolean\"},"
    "\"disc_reason\":{\"type\":\"integer\"},"
    "\"disc_age_s\":{\"type\":\"integer\"},"
    "\"retry_count\":{\"type\":\"integer\"}}},"
    "\"http_handler_count\":{\"type\":\"integer\"},"
    "\"http_handler_cap\":{\"type\":\"integer\"},"
    "\"uptime_ms\":{\"type\":\"integer\"},"
    "\"boot_epoch_s\":{\"type\":\"integer\"},"
    "\"time_valid\":{\"type\":\"boolean\"},"
    "\"time_source\":{\"type\":\"string\"},"
    "\"hostname\":{\"type\":[\"string\",\"null\"]},"
    "\"capabilities\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}}";

// Suffix: the section properties (display, led, ntp, diag, ...) are inserted
// between base and suffix by bb_section_assemble_schema with commas.
// The suffix closes the properties object and adds the required array.
static const char k_info_schema_suffix[] =
    "},"
    "\"required\":[\"board\",\"version\",\"network\"]}";
