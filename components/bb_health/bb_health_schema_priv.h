#pragma once

// Private: shared between platform/espidf and platform/host bb_health implementations.
//
// bb_section_assemble_schema(reg, k_health_base, k_health_suffix) builds the
// complete /api/health 200 JSON-Schema:
//   k_health_base + [,"<name>":<schema_props>]* + k_health_suffix
//
// Root-level fields (ok/free_heap/validated/network) live in base/suffix.
// Named sections (mqtt, temp, ...) are emitted by bb_section_assemble_schema
// from each section's schema_props string.
//
// NOTE: bb_section_assemble_schema detects that base ends with non-'{' content
// and prepends a leading ',' before the first section automatically.

static const char k_health_base[] =
    "{\"type\":\"object\","
    "\"properties\":{"
    "\"ok\":{\"type\":\"boolean\"},"
    "\"validated\":{\"type\":\"boolean\"},"
    "\"network\":{\"type\":\"object\","
    "\"properties\":{"
    "\"ssid\":{\"type\":\"string\"},"
    "\"bssid\":{\"type\":\"string\"},"
    "\"ip\":{\"type\":\"string\"},"
    "\"connected\":{\"type\":\"boolean\"},"
    "\"mdns\":{\"type\":[\"string\",\"null\"]}}}";

// Suffix: section properties (mqtt, temp, ...) are inserted between base and suffix
// by bb_section_assemble_schema with commas. Suffix closes the properties object
// and adds the required array.
static const char k_health_suffix[] =
    "},"
    "\"required\":[\"ok\",\"network\"]}";
