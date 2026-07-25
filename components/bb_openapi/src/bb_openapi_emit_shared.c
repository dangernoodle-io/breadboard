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
#include "bb_log.h"

#include <string.h>
#include <ctype.h>
#include <stdio.h>

static const char *TAG = "bb_openapi_emit_shared";

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

// ---------------------------------------------------------------------------
// Single dedup rule for the whole schema union (B1-1220 follow-up review
// fix) -- shared by ALL FOUR union consumers below: sse_schema_count() /
// build_sse_oneof_fragment() (the oneOf array) AND
// bb_openapi_schemas_union_count() / emit_external_schemas()
// (components/schemas). One rule: dedup by component_name, legacy-first-
// wins, across the WHOLE union -- legacy vs external AND external vs
// external.
//
// Why external-vs-external matters: bb_data_http_describe() (the seam's
// intended external source) dedups by `key` (bb_data_http.h), NOT by
// component_name -- two distinct topics/keys can legitimately share one
// component_name. Without also deduping external-vs-external, that
// collision double-counts, double-emits a components/schemas JSON key
// (invalid JSON — a duplicate object key), AND double-emits the same $ref
// into oneOf (which means EXACTLY ONE subschema matches; two identical
// branches breaks that contract, not just cosmetically).
//
// component_name IS the dedup axis, not a policy choice: it is the literal
// object key under components/schemas, and a JSON object cannot hold two
// values under one key -- there is no legitimate case for two bodies under
// one component_name to coexist in the emitted document, so first-wins
// (matching bb_openapi_register_schema()'s own convention below) is the
// only correct resolution, same as the legacy registry's own dedup.
// ---------------------------------------------------------------------------

// Dedup helper used by schema_already_emitted() below: true if component_name
// already has a legacy-registry entry. Legacy wins on collision -- the same
// first-wins convention bb_openapi_register_schema() itself uses for
// re-registering an already-known component_name (see its own dedup loop
// above).
static bool schema_registry_contains(const char *component_name)
{
    for (size_t i = 0; i < s_schema_count; i++) {
        if (strcmp(s_schema_registry[i].component_name, component_name) == 0) return true;
    }
    return false;
}

// Own cap for the "seen so far in THIS external walk" scratch below -- NOT
// tied to bb_data_http's BB_DATA_HTTP_MAX_DESCRIBE (bb_openapi has no
// linkage to that header, by design -- see bb_openapi.h's seam doc
// comment). Sized generously past bb_data_http's current cap (8, per
// bb_data_http.h) with headroom for a different/future external source. A
// source producing more DISTINCT component_names than this within one walk
// degrades gracefully rather than heap-growing or crashing: names past the
// cap are simply not recorded, so a duplicate of one of THOSE names could
// re-emit -- the same kind of explicit, documented bound this file already
// uses for the oneOf buffer (BB_OPENAPI_SSE_ONEOF_BUF_SIZE above).
#define BB_OPENAPI_EXTERNAL_DEDUP_CAP 16

// Walk-scoped "which external component_names have I already accepted"
// scratch -- stack-allocated by each of the four call sites below, never
// heap. Legacy names are NOT stored here (schema_registry_contains() above
// already covers legacy-vs-external directly against the live registry), so
// this only needs to bound the EXTERNAL side of the union.
typedef struct {
    const char *seen[BB_OPENAPI_EXTERNAL_DEDUP_CAP];
    size_t      count;
    bool        overflow_logged;  // one-shot: see schema_dedup_mark() below
} schema_dedup_t;

// The single "have I already emitted this component_name" predicate shared
// by every union consumer: true if component_name has a legacy-registry
// entry OR was already accepted earlier in THIS SAME external walk
// (registration-order first-wins across external entries too, not just
// legacy-vs-external).
static bool schema_already_emitted(const schema_dedup_t *dedup, const char *component_name)
{
    if (schema_registry_contains(component_name)) return true;
    for (size_t i = 0; i < dedup->count; i++) {
        if (strcmp(dedup->seen[i], component_name) == 0) return true;
    }
    return false;
}

// Records component_name as emitted for the remainder of this walk. No-op
// (dedup tracking, not the schema itself) once the scratch is full -- see
// BB_OPENAPI_EXTERNAL_DEDUP_CAP's doc comment above for the accepted degrade
// behavior: past the cap, a repeat of an untracked name can re-emit a
// duplicate components/schemas key and oneOf $ref. Silent degradation here
// would be the wrong failure mode for a bound that exists only because
// bb_openapi deliberately cannot see the external source's own cap, so log
// once per walk (the `overflow_logged` sticky flag) rather than staying
// silent or spamming once per over-cap entry.
static void schema_dedup_mark(schema_dedup_t *dedup, const char *component_name)
{
    if (dedup->count >= BB_OPENAPI_EXTERNAL_DEDUP_CAP) {
        if (!dedup->overflow_logged) {
            bb_log_e(TAG, "external schema dedup cap (%d) exceeded — dedup "
                          "degraded, a repeated component_name past the cap "
                          "may re-emit a duplicate components/schemas entry",
                     BB_OPENAPI_EXTERNAL_DEDUP_CAP);
            dedup->overflow_logged = true;
        }
        return;
    }
    dedup->seen[dedup->count++] = component_name;
}

// bb_openapi_topic_source_invoke() callback for sse_schema_count() below --
// every entry the external source walks counts as an SSE-facing schema
// (unlike the legacy registry, an external source has no REST-only/
// NULL-topic member: every entry it walks names a topic by construction --
// see bb_data_http_describe()'s own argument validation) UNLESS its
// component_name is already covered (legacy or an earlier external entry in
// this same walk -- schema_already_emitted() above).
typedef struct {
    schema_dedup_t dedup;
    size_t          count;
} sse_count_ctx_t;

static void count_topic_source_entry(const char *key, const char *topic,
                                     const char *component_name,
                                     const char *schema_literal, void *ctx)
{
    (void)key; (void)topic; (void)schema_literal;
    sse_count_ctx_t *c = (sse_count_ctx_t *)ctx;
    if (schema_already_emitted(&c->dedup, component_name)) return;
    schema_dedup_mark(&c->dedup, component_name);
    c->count++;
}

// Count of registered schemas with a non-NULL sse_topic, PLUS every
// NOT-already-covered entry the external topic-source seam (if wired) walks
// -- the two sources are unioned here and in build_sse_oneof_fragment()
// below (legacy registry walked first, external source second, so ordering
// stays stable as producers migrate from bb_openapi_register_topic_schema()
// to an external source one at a time). Unset (the default) is a no-op
// union: behavior is byte-identical to pre-B1-1220.
size_t sse_schema_count(void)
{
    size_t n = 0;
    for (size_t k = 0; k < s_schema_count; k++) {
        if (s_schema_registry[k].sse_topic) n++;
    }
    sse_count_ctx_t ctx = { .dedup = {0}, .count = 0 };
    bb_openapi_topic_source_invoke(count_topic_source_entry, &ctx);
    return n + ctx.count;
}

// ---------------------------------------------------------------------------
// components/schemas union (B1-1220 follow-up fix) -- PR1080 wired the
// injected topic-source seam into sse_schema_count()/build_sse_oneof_
// fragment() above (the SSE route's oneOf array) only. components/schemas
// itself (bb_openapi_emit.c) was never updated to consult the seam, so a
// producer migrating from bb_openapi_register_topic_schema() to
// bb_data_http_describe() would gain a $ref in oneOf with no matching
// components/schemas body -- a dangling $ref, an invalid OpenAPI document.
// bb_openapi_schemas_union_count()/emit_external_schemas() below close that
// gap the same way sse_schema_count()/build_sse_oneof_fragment() already
// close it for oneOf: legacy registry first, external source second, same
// shared dedup rule (schema_already_emitted() above).
// ---------------------------------------------------------------------------

// bb_openapi_topic_source_invoke() callback for bb_openapi_schemas_union_count()
// below -- counts only external entries not already covered (legacy or an
// earlier external entry in this same walk).
typedef struct {
    schema_dedup_t dedup;
    size_t          count;
} schema_count_ctx_t;

static void count_external_schema_entry(const char *key, const char *topic,
                                        const char *component_name,
                                        const char *schema_literal, void *ctx)
{
    (void)key; (void)topic; (void)schema_literal;
    schema_count_ctx_t *c = (schema_count_ctx_t *)ctx;
    if (schema_already_emitted(&c->dedup, component_name)) return;
    schema_dedup_mark(&c->dedup, component_name);
    c->count++;
}

size_t bb_openapi_schemas_union_count(void)
{
    schema_count_ctx_t ctx = { .dedup = {0}, .count = s_schema_count };
    bb_openapi_topic_source_invoke(count_external_schema_entry, &ctx);
    return ctx.count;
}

// bb_openapi_topic_source_invoke() callback for emit_external_schemas()
// below -- streams one components/schemas entry per external-source entry
// not already covered (same dedup as bb_openapi_schemas_union_count()
// above).
typedef struct {
    bb_http_json_obj_stream_t *stream;
    schema_dedup_t             dedup;
} external_schema_emit_ctx_t;

static void emit_external_schema_entry(const char *key, const char *topic,
                                       const char *component_name,
                                       const char *schema_literal, void *ctx)
{
    (void)key; (void)topic;
    external_schema_emit_ctx_t *ec = (external_schema_emit_ctx_t *)ctx;
    if (schema_already_emitted(&ec->dedup, component_name)) return;
    schema_dedup_mark(&ec->dedup, component_name);
    bb_http_resp_json_obj_set_raw(ec->stream, component_name, schema_literal,
                                  strlen(schema_literal));
}

void emit_external_schemas(bb_http_json_obj_stream_t *s)
{
    external_schema_emit_ctx_t ec = { .stream = s, .dedup = {0} };
    bb_openapi_topic_source_invoke(emit_external_schema_entry, &ec);
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
// `dedup` applies the same shared dedup rule (schema_already_emitted()
// above) to the external-source side of this cursor's use (see
// append_topic_source_ref() below) -- the legacy-registry loop itself never
// needs it (that registry is already unique by construction).
typedef struct {
    char   *out;
    size_t  out_size;
    size_t  pos;
    bool    first;
    bool    overflow;  // sticky — once true, append_ref() is a no-op
    schema_dedup_t dedup;
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
// uses — but ONLY if component_name isn't already covered (legacy or an
// earlier external entry in this same walk, cur->dedup) -- see
// sse_schema_count()'s doc comment for the union/ordering rationale (legacy
// first, external source second) and the shared-dedup-rule comment above
// schema_already_emitted() for why external-vs-external matters here: two
// identical $ref branches in a oneOf array violate its "exactly one match"
// semantics, not just duplicate text.
static void append_topic_source_ref(const char *key, const char *topic,
                                    const char *component_name,
                                    const char *schema_literal, void *ctx)
{
    (void)key; (void)topic; (void)schema_literal;
    oneof_cursor_t *cur = (oneof_cursor_t *)ctx;
    if (schema_already_emitted(&cur->dedup, component_name)) return;
    schema_dedup_mark(&cur->dedup, component_name);
    append_ref(cur, component_name);
}

bool build_sse_oneof_fragment(char *out, size_t out_size)
{
    int n = snprintf(out, out_size, "{\"oneOf\":[");
    if ((size_t)n >= out_size) return false;

    oneof_cursor_t cur = { .out = out, .out_size = out_size, .pos = (size_t)n,
                          .first = true, .overflow = false, .dedup = {0} };

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
