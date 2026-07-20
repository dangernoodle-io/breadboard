#pragma once
// Private shared header for the bb_http_section namespace registry
// (bb_http_section PR). The pure, portable dispatch helper (longest-prefix
// namespace lookup + name-strip) factored out of the ESP-IDF-only
// dispatcher (platform/espidf/bb_http_server/bb_http_section_dispatch.c) so
// both it and a host test can exercise the SAME code path -- mirrors
// bb_diag_section_priv.h's split. No ESP-IDF or FreeRTOS types here.
// Included by:
//   - components/bb_http_server/src/bb_http_section.c
//   - platform/espidf/bb_http_server/bb_http_section_dispatch.c
//   - test/test_host/test_bb_http_section.c

#include "bb_http_section.h"

#include <stddef.h>

// Bounds a request's stripped URI path scratch buffer: the longest
// registered prefix plus a name up to BB_HTTP_SECTION_NAME_MAX - 1 bytes,
// plus NUL. Compile-time-only, not a per-target Kconfig tunable.
#define BB_HTTP_SECTION_URI_MAX (BB_HTTP_SECTION_PREFIX_MAX + BB_HTTP_SECTION_NAME_MAX)

// Finds the registered namespace whose prefix is the LONGEST match for
// `uri` (query string, if any, already stripped by the caller -- same
// contract bb_diag_section_name_from_uri() and bb_http_req_uri() itself
// use), and copies the "<name>" segment following that prefix into `out`
// (capacity `out_cap`), NUL-terminated, truncated if it doesn't fit.
//
// Returns the matched namespace, or NULL if no registered prefix matches
// `uri` (`out` is left untouched in that case) or if `uri`, `out` is NULL,
// or `out_cap` is 0.
const bb_http_section_ns_t *bb_http_section_find(const char *uri, char *out, size_t out_cap);

// Returns the number of namespaces currently registered via
// bb_http_section_register_ns(). Used by the ESP-IDF dispatcher
// (bb_http_section_init()) to walk the table and register one GET + one
// PATCH route per namespace.
size_t bb_http_section_count(void);

// Returns namespace `idx`'s registration (idx MUST be < bb_http_section_count())
// plus its precomputed "<prefix>*" wildcard route string via `*out_wildcard`
// -- static-storage lifetime (owned by the registry's own table), safe to
// hand directly to bb_http_register_route(), which stores the raw pointer
// rather than copying it.
const bb_http_section_ns_t *bb_http_section_at(size_t idx, const char **out_wildcard);
