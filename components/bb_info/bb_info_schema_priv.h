#pragma once

// Private: shared between platform/espidf and platform/host bb_info implementations.
// Split of the /api/info 200 JSON-Schema into base + suffix so extender fragments
// can be injected between them at init time.
//
// Assembled result (no extenders):
//   k_info_schema_base + k_info_schema_suffix
// must equal the original monolithic literal plus the http_handler_count and
// http_handler_cap properties that were previously missing from the schema.
//
// Fragment injection (N extenders):
//   k_info_schema_base + "," + frag[0] + "," + frag[1] + ... + k_info_schema_suffix

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
    "\"total_heap\":{\"type\":\"integer\"},"
    "\"free_heap\":{\"type\":\"integer\"},"
    "\"app_size\":{\"type\":\"integer\"},"
    "\"reset_reason\":{\"type\":\"string\"},"
    "\"ota_validated\":{\"type\":\"boolean\"},"
    "\"heap_free_total\":{\"type\":\"integer\"},"
    "\"heap_free_internal\":{\"type\":\"integer\"},"
    "\"heap_minimum_ever\":{\"type\":\"integer\"},"
    "\"heap_largest_free_block\":{\"type\":\"integer\"},"
    "\"chip_revision\":{\"type\":\"integer\"},"
    "\"cpu_freq_mhz\":{\"type\":\"integer\"},"
    "\"heap_internal\":{\"type\":\"object\","
    "\"properties\":{"
    "\"free\":{\"type\":\"integer\"},"
    "\"total\":{\"type\":\"integer\"}}},"
    "\"heap_psram\":{\"type\":\"object\","
    "\"properties\":{"
    "\"free\":{\"type\":\"integer\"},"
    "\"total\":{\"type\":\"integer\"}}},"
    "\"rtc\":{\"type\":\"object\","
    "\"properties\":{"
    "\"used\":{\"type\":\"integer\"},"
    "\"total\":{\"type\":\"integer\"}}},"
    "\"network\":{\"type\":\"object\","
    "\"properties\":{"
    "\"ssid\":{\"type\":\"string\"},"
    "\"bssid\":{\"type\":\"string\"},"
    "\"rssi\":{\"type\":\"integer\"},"
    "\"ip\":{\"type\":\"string\"},"
    "\"connected\":{\"type\":\"boolean\"},"
    "\"disc_reason\":{\"type\":\"integer\"},"
    "\"disc_age_s\":{\"type\":\"integer\"},"
    "\"retry_count\":{\"type\":\"integer\"}}}";

// Suffix closes the properties object and adds the required array.
// Also documents http_handler_count and http_handler_cap which are emitted
// by info_handler but were previously absent from the schema.
static const char k_info_schema_suffix[] =
    ",\"http_handler_count\":{\"type\":\"integer\"},"
    "\"http_handler_cap\":{\"type\":\"integer\"}"
    "},"
    "\"required\":[\"board\",\"version\",\"network\"]}";
