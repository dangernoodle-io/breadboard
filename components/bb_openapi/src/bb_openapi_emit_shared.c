// Plumbing shared by both bb_openapi emitter translation units — the schema
// registry, method/operationId derivation, path-uniqueness tracking, and the
// SSE oneOf fragment builder. Consumed by:
//   - bb_openapi_emit.c (device streaming emitter, ships in firmware)
//   - platform/host/bb_openapi/bb_openapi_emit_tree.c (host-only tree
//     emitter, never linked into firmware)
// See bb_openapi_emit_internal.h for the shared declarations.
#include "bb_openapi.h"
#include "bb_openapi_emit_internal.h"

#include <string.h>
#include <ctype.h>
#include <stdio.h>

// ---------------------------------------------------------------------------
// Schema component registry
// ---------------------------------------------------------------------------

typedef struct {
    const char *component_name;
    const char *schema_literal;
    const char *sse_topic;
} schema_entry_t;

static schema_entry_t s_schema_registry[BB_OPENAPI_SCHEMA_REGISTRY_CAP];
static size_t         s_schema_count = 0;

bb_err_t bb_openapi_register_schema(const char *component_name,
                                    const char *schema_literal,
                                    const char *sse_topic)
{
    if (!component_name || !schema_literal) return BB_ERR_INVALID_ARG;
    for (size_t i = 0; i < s_schema_count; i++) {
        if (strcmp(s_schema_registry[i].component_name, component_name) == 0) {
            return BB_OK;
        }
    }
    if (s_schema_count >= BB_OPENAPI_SCHEMA_REGISTRY_CAP) return BB_ERR_NO_SPACE;
    s_schema_registry[s_schema_count].component_name = component_name;
    s_schema_registry[s_schema_count].schema_literal = schema_literal;
    s_schema_registry[s_schema_count].sse_topic      = sse_topic;
    s_schema_count++;
    return BB_OK;
}

bb_err_t bb_openapi_register_topic_schema(const char *topic_name,
                                          const char *schema_literal,
                                          const char *component_name)
{
    return bb_openapi_register_schema(component_name, schema_literal, topic_name);
}

void bb_openapi_schema_registry_clear(void)
{
    s_schema_count = 0;
}

size_t bb_openapi_schema_count(void)
{
    return s_schema_count;
}

bool bb_openapi_schema_get(size_t idx, bb_openapi_schema_entry_t *out)
{
    if (idx >= s_schema_count || !out) return false;
    out->component_name = s_schema_registry[idx].component_name;
    out->schema_literal = s_schema_registry[idx].schema_literal;
    out->sse_topic      = s_schema_registry[idx].sse_topic;
    return true;
}

size_t sse_schema_count(void)
{
    size_t n = 0;
    for (size_t k = 0; k < s_schema_count; k++) {
        if (s_schema_registry[k].sse_topic) n++;
    }
    return n;
}

// ---------------------------------------------------------------------------
// Method name helpers
// ---------------------------------------------------------------------------

const char *method_str(bb_http_method_t m)
{
    switch (m) {
        case BB_HTTP_GET:     return "get";
        case BB_HTTP_POST:    return "post";
        case BB_HTTP_PATCH:   return "patch";
        case BB_HTTP_PUT:     return "put";
        case BB_HTTP_DELETE:  return "delete";
        case BB_HTTP_OPTIONS: return "options";
        default:              return "get";
    }
}

// ---------------------------------------------------------------------------
// operationId derivation
// ---------------------------------------------------------------------------

void derive_operation_id(bb_http_method_t method, const char *path,
                         char *out, size_t out_size)
{
    const char *m = method_str(method);
    size_t pos = 0;

    // copy method prefix (already lowercase). method_str returns at most
    // 7 chars ("options"); out_size is 128 — no bounds check needed here.
    for (const char *p = m; *p; p++) {
        out[pos++] = *p;
    }

    bool next_upper = true;  // first path char after '/' gets uppercased
    bool skip_slash = true;  // skip the leading '/'

    for (const char *p = path; *p && pos < out_size - 1; p++) {
        char c = *p;
        if (c == '/') {
            if (skip_slash) {
                skip_slash = false;
            } else {
                next_upper = true;
            }
            continue;
        }
        if (c == '-' || c == '_') {
            next_upper = true;
            continue;
        }
        if (next_upper) {
            out[pos++] = (char)toupper((unsigned char)c);
            next_upper = false;
        } else {
            out[pos++] = c;
        }
    }

    out[pos] = '\0';
}

// ---------------------------------------------------------------------------
// Path uniqueness tracking (stack array, no malloc)
// ---------------------------------------------------------------------------

bool path_set_contains(const path_set_t *ps, const char *path)
{
    for (size_t i = 0; i < ps->count; i++) {
        if (strcmp(ps->paths[i], path) == 0) return true;
    }
    return false;
}

void path_set_add(path_set_t *ps, const char *path)
{
    // The bb_http registry caps at BB_ROUTE_REGISTRY_CAP (64) == UNIQUE_PATH_CAP,
    // so the path_set can hold every distinct path the registry can store.
    if (!path_set_contains(ps, path)) {
        ps->paths[ps->count++] = path;
    }
}

void collect_paths_walker(const bb_route_t *route, void *ctx)
{
    path_set_t *ps = (path_set_t *)ctx;
    path_set_add(ps, route->path);
}

// ---------------------------------------------------------------------------
// SSE oneOf fragment builder
// ---------------------------------------------------------------------------
// BB_OPENAPI_SSE_REF_BUF_SIZE / BB_OPENAPI_SSE_ONEOF_BUF_SIZE are defined in
// bb_openapi_emit_internal.h (included above) — shared with host tests that
// size buffers against them via the BB_OPENAPI_TESTING seam.

// Every "did this snprintf fit" check below is a single unsigned comparison
// rather than `ret < 0 || (size_t)ret >= size`: snprintf only returns
// negative on an output/encoding error, which cannot happen writing plain
// ASCII into these buffers, so a separate `ret < 0` branch would be dead —
// untestable without faking libc — and would sit as a permanently-missed
// branch on every coverage run. Casting a hypothetical negative return to
// size_t wraps it to a huge value, which trivially satisfies `>= size`
// anyway, so the single comparison is both correct and branch-free for
// that case (nothing to exclude because there is no separate edge).
bool build_sse_oneof_fragment(char *out, size_t out_size)
{
    int n = snprintf(out, out_size, "{\"oneOf\":[");
    if ((size_t)n >= out_size) return false;
    size_t pos = (size_t)n;

    bool first = true;
    for (size_t k = 0; k < s_schema_count; k++) {
        if (!s_schema_registry[k].sse_topic) continue;

        char ref_val[BB_OPENAPI_SSE_REF_BUF_SIZE];
        int ref_n = snprintf(ref_val, sizeof(ref_val), "#/components/schemas/%s",
                             s_schema_registry[k].component_name);
        // A silently truncated $ref would splice a WRONG (but well-formed)
        // component reference into otherwise-valid JSON — worse than the
        // malformed-JSON case the outer overflow guard exists to prevent.
        // Treat it the same way: omit the whole fragment rather than emit it.
        if ((size_t)ref_n >= sizeof(ref_val)) return false;

        int written = snprintf(out + pos, out_size - pos,
                               "%s{\"$ref\":\"%s\"}", first ? "" : ",", ref_val);
        if ((size_t)written >= out_size - pos) return false;
        pos += (size_t)written;
        first = false;
    }

    int closing = snprintf(out + pos, out_size - pos, "]}");
    if ((size_t)closing >= out_size - pos) return false;

    return true;
}

#ifdef BB_OPENAPI_TESTING
// Test-only seam: exposes the fragment builder so host tests can drive it
// directly with a caller-controlled out_size (the cheapest lever for
// hitting its overflow branches — see test_sse_schema_fidelity.c), same
// "_for_test" + BB_<COMPONENT>_TESTING convention used elsewhere in this
// codebase (e.g. bb_ota_check_config_desc_for_test()).
bool bb_openapi_build_sse_oneof_fragment_for_test(char *out, size_t out_size)
{
    return build_sse_oneof_fragment(out, out_size);
}
#endif /* BB_OPENAPI_TESTING */
