// Plumbing for bb_openapi_emit.c (device streaming emitter, the sole
// consumer) — the schema registry, method/operationId derivation,
// path-uniqueness tracking, and the SSE oneOf fragment builder. A
// host-only tree emitter (platform/host/bb_openapi/bb_openapi_emit_tree.c)
// used to share this plumbing too; it was removed entirely in B1-1054 once
// bb_openapi_emit.c reached full parity, leaving bb_openapi_emit.c as the
// only consumer.
// See bb_openapi_emit_internal.h for the shared declarations.
#include "bb_openapi.h"
#include "bb_openapi_emit_internal.h"
#include "bb_callback_slot.h"

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

// ---------------------------------------------------------------------------
// External topic-schema source seam (B1-1220 PR2, bb_openapi.h). bb_openapi
// never links bb_data_http (or any other producer component) directly --
// this installable fn pointer is how a composition root points the union
// below at an external topic-schema table (e.g. bb_data_http's describe
// table, via bb_data_http_describe_foreach()) without bb_openapi taking a
// dependency on it. NULL default (bb_openapi_topic_source_invoke() below is
// a null-safe no-op): sse_schema_count()/build_sse_oneof_fragment() below
// fall back to the legacy registry alone, byte-identical to pre-B1-1220
// behavior -- same "degrades gracefully when unset" contract bb_data_http's
// own render/generation/send seams use.
//
// BB_CALLBACK_SLOT_VOID (bb_callback_slot.h, the consolidated "single-slot
// injected callback" idiom -- see its own header for the fence this is
// draining) rather than a hand-rolled static+setter: this is a void-return,
// args-carrying seam, exactly the shape that macro covers. `entry_cb` (not
// `cb`) names decl_args'/call_args' own callback parameter to avoid shadowing
// the macro-generated invoke's internal `cb_type cb` local. bb_openapi_set_
// topic_source_fn is declared in bb_openapi.h (public API); the generated
// invoke, bb_openapi_topic_source_invoke, is used only within this file
// (below) and needs no separate prototype -- mirrors platform/host/bb_wifi/
// bb_wifi_emit.c's BB_CALLBACK_SLOT_RET/VOID_CTX instantiations.
// ---------------------------------------------------------------------------
BB_CALLBACK_SLOT_VOID(topic_source, bb_openapi_topic_source_fn_t,
                      bb_openapi_set_topic_source_fn, bb_openapi_topic_source_invoke,
                      (bb_openapi_topic_cb_t entry_cb, void *ctx), (entry_cb, ctx))

// bb_openapi_topic_source_invoke() callback for sse_schema_count() below -- every entry
// the external source walks counts as an SSE-facing schema (unlike the
// legacy registry, an external source has no REST-only/NULL-topic member:
// every entry it walks names a topic by construction -- see
// bb_data_http_describe()'s own argument validation).
static void count_topic_source_entry(const char *key, const char *topic,
                                     const char *component_name,
                                     const char *schema_literal, void *ctx)
{
    (void)key; (void)topic; (void)component_name; (void)schema_literal;
    (*(size_t *)ctx)++;
}

// Count of registered schemas with a non-NULL sse_topic, PLUS every entry
// the external topic-source seam (if wired) walks -- the two sources are
// unioned here and in build_sse_oneof_fragment() below (legacy registry
// walked first, external source second, so ordering stays stable as
// producers migrate from bb_openapi_register_topic_schema() to an external
// source one at a time). Unset (the default) is a no-op union: behavior is
// byte-identical to pre-B1-1220.
size_t sse_schema_count(void)
{
    size_t n = 0;
    for (size_t k = 0; k < s_schema_count; k++) {
        if (s_schema_registry[k].sse_topic) n++;
    }
    bb_openapi_topic_source_invoke(count_topic_source_entry, &n);
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

// Shared mutable cursor for the two ref-appending call sites below (the
// legacy-registry loop and the bb_data_http describe-table foreach) — one
// idiom, one place it can overflow, rather than duplicating the same
// snprintf/bounds-check pair per source (B1-1220 second use of this exact
// idiom; consolidation convention says extract on the second instance).
typedef struct {
    char   *out;
    size_t  out_size;
    size_t  pos;
    bool    first;
    bool    overflow;  // sticky — once true, append_ref() is a no-op
} oneof_cursor_t;

static void append_ref(oneof_cursor_t *cur, const char *component_name)
{
    if (cur->overflow) return;

    char ref_val[BB_OPENAPI_SSE_REF_BUF_SIZE];
    int ref_n = snprintf(ref_val, sizeof(ref_val), "#/components/schemas/%s",
                         component_name);
    // A silently truncated $ref would splice a WRONG (but well-formed)
    // component reference into otherwise-valid JSON — worse than the
    // malformed-JSON case the outer overflow guard exists to prevent.
    // Treat it the same way: omit the whole fragment rather than emit it.
    if ((size_t)ref_n >= sizeof(ref_val)) { cur->overflow = true; return; }

    int written = snprintf(cur->out + cur->pos, cur->out_size - cur->pos,
                           "%s{\"$ref\":\"%s\"}", cur->first ? "" : ",", ref_val);
    if ((size_t)written >= cur->out_size - cur->pos) { cur->overflow = true; return; }
    cur->pos += (size_t)written;
    cur->first = false;
}

// bb_openapi_topic_source_invoke() callback: appends one $ref per
// external-source entry onto the same cursor the legacy-registry loop below
// uses — see sse_schema_count()'s doc comment for the union/ordering
// rationale (legacy first, external source second).
static void append_topic_source_ref(const char *key, const char *topic,
                                    const char *component_name,
                                    const char *schema_literal, void *ctx)
{
    (void)key; (void)topic; (void)schema_literal;
    append_ref((oneof_cursor_t *)ctx, component_name);
}

bool build_sse_oneof_fragment(char *out, size_t out_size)
{
    int n = snprintf(out, out_size, "{\"oneOf\":[");
    if ((size_t)n >= out_size) return false;

    oneof_cursor_t cur = { .out = out, .out_size = out_size, .pos = (size_t)n,
                          .first = true, .overflow = false };

    for (size_t k = 0; k < s_schema_count; k++) {
        if (!s_schema_registry[k].sse_topic) continue;
        append_ref(&cur, s_schema_registry[k].component_name);
        if (cur.overflow) return false;
    }

    bb_openapi_topic_source_invoke(append_topic_source_ref, &cur);
    if (cur.overflow) return false;

    int closing = snprintf(out + cur.pos, out_size - cur.pos, "]}");
    if ((size_t)closing >= out_size - cur.pos) return false;

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
