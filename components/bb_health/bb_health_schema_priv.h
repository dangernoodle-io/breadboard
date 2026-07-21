#pragma once

// Private: shared between platform/espidf and platform/host bb_health implementations.
//
// bb_health_assemble_schema() (bb_health_schema.c) builds the complete
// /api/health 200 JSON-Schema:
//   k_health_base + [,"<name>":<schema_props>]* + k_health_suffix
//
// Root-level fields (ok/validated/network) live in base/suffix. Named
// sections (mqtt, temp, ...) are emitted from each bb_health_section's
// schema_props string, walked via bb_health_section_count()/
// bb_health_section_get_by_index() (B1-1100 -- repoints schema assembly
// off the retired bb_response registry onto the bb_health_section
// composer).
//
// k_health_base always ends with non-'{' content (the closing "}}" of the
// network object), so every section gets an unconditional leading ','
// (unlike bb_response_assemble_schema()'s general base-ends-with-'{' check,
// not needed here since this base is a fixed, known-non-empty string).

#include "bb_core.h"

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

// Suffix: section properties (mqtt, temp, ...) are inserted between base and
// suffix by bb_health_assemble_schema() with leading commas. Suffix closes
// the properties object and adds the required array.
static const char k_health_suffix[] =
    "},"
    "\"required\":[\"ok\",\"network\"]}";

// Assembles the /api/health 200 JSON-Schema from k_health_base + every
// currently-registered bb_health_section's schema_props (sections with a
// NULL schema_props are omitted) + k_health_suffix. Heap-allocated
// (init-time only, mirrors bb_response_assemble_schema()'s retired
// contract) -- caller must free() the result. Returns NULL on OOM.
char *bb_health_assemble_schema(void);

#ifdef BB_HEALTH_TESTING
// Test-only: override the malloc bb_health_assemble_schema() uses (mirrors
// bb_response_set_malloc()) -- lets a host test force the OOM path. Pass
// NULL to restore the real malloc().
void bb_health_schema_set_malloc(void *(*m)(size_t));
#endif
