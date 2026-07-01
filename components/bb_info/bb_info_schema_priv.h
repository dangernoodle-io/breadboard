#pragma once

// Private: shared between platform/espidf and platform/host bb_info implementations.
//
// bb_response_assemble_schema(reg, k_info_schema_base, k_info_schema_suffix) builds the
// complete /api/info 200 JSON-Schema:
//   k_info_schema_base + [,"<name>":<schema_props>]* + k_info_schema_suffix
//
// Root-level fields (board, network, capabilities, http_handler_*) live in base/suffix.
// Named sections (display, led, ntp, diag) are emitted by bb_response_assemble_schema
// from each section's schema_props string.
//
// NOTE: abnormal_reset_count and panic have been REMOVED from root.
//       They live in the diag section: diag.{wdt_resets, panic?}.

// Schema for the nested "build" subsection (bb_cache "build" topic).
// 13 fields: the 12 formerly at root level + app_sha256.
static const char k_build_schema[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"version\":{\"type\":\"string\"},"
    "\"idf_version\":{\"type\":\"string\"},"
    "\"build_date\":{\"type\":\"string\"},"
    "\"build_time\":{\"type\":\"string\"},"
    "\"project_name\":{\"type\":\"string\"},"
    "\"chip_model\":{\"type\":\"string\"},"
    "\"chip_revision\":{\"type\":\"integer\"},"
    "\"cores\":{\"type\":\"integer\"},"
    "\"cpu_freq_mhz\":{\"type\":\"integer\"},"
    "\"flash_size\":{\"type\":\"integer\"},"
    "\"app_size\":{\"type\":\"integer\"},"
    "\"board\":{\"type\":\"string\"},"
    "\"app_sha256\":{\"type\":\"string\"}}}";

static const char k_info_schema_base[] =
    "{\"type\":\"object\","
    "\"properties\":{"
    "\"mac\":{\"type\":\"string\"},"
    "\"ota_validated\":{\"type\":\"boolean\"},"
    "\"ota_ready\":{\"type\":\"boolean\"},"
    "\"boot_epoch_s\":{\"type\":\"integer\"},"
    "\"time_valid\":{\"type\":\"boolean\"},"
    "\"time_source\":{\"type\":\"string\"},"
    "\"hostname\":{\"type\":[\"string\",\"null\"]},"
    "\"capabilities\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}}";

// Suffix: the section properties (display, led, ntp, diag, ...) are inserted
// between base and suffix by bb_response_assemble_schema with commas.
// The suffix closes the properties object and adds the required array.
static const char k_info_schema_suffix[] =
    "},"
    "\"required\":[\"mac\",\"capabilities\"]}";
