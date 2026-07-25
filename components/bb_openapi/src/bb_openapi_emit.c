#include "bb_openapi.h"
#include "bb_openapi_emit_internal.h"
#include "bb_log.h"

#include <stdbool.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>

static const char *TAG = "bb_openapi_emit";

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

#define UNIQUE_PATH_CAP 64

// ---------------------------------------------------------------------------
// Method name helpers
// ---------------------------------------------------------------------------

static const char *method_str(bb_http_method_t m)
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
// Rule: <method><PathCamelCase>
//   - strip leading '/'
//   - each path segment's first char is uppercased
//   - '-' and '_' are dropped and next char is uppercased
//   e.g. GET /api/stats -> "getApiStats"
//        POST /api/pool-config -> "postApiPoolConfig"

// Caller guarantees: non-NULL path (registry walkers skip NULL-path routes
// in collect_paths_walker / emit_operations_walker) and a non-empty buffer
// (only call site uses a fixed 128-byte stack array).
static void derive_operation_id(bb_http_method_t method, const char *path,
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

typedef struct {
    const char *paths[UNIQUE_PATH_CAP];
    size_t      count;
} path_set_t;

static bool path_set_contains(const path_set_t *ps, const char *path)
{
    for (size_t i = 0; i < ps->count; i++) {
        if (strcmp(ps->paths[i], path) == 0) return true;
    }
    return false;
}

static void path_set_add(path_set_t *ps, const char *path)
{
    // The bb_http registry caps at BB_ROUTE_REGISTRY_CAP (64) == UNIQUE_PATH_CAP,
    // so the path_set can hold every distinct path the registry can store.
    if (!path_set_contains(ps, path)) {
        ps->paths[ps->count++] = path;
    }
}

// ---------------------------------------------------------------------------
// Walker context for two-pass emission
// ---------------------------------------------------------------------------

typedef struct {
    path_set_t          *path_set;
    bb_json_t            paths_obj;
    const char          *current_path;
    bb_json_t            path_item_obj;
} emit_ctx_t;

// Pass 1: collect unique paths.
// bb_http_register_described_route rejects NULL routes; descriptors stored in
// the registry are guaranteed non-NULL with non-NULL paths.
static void collect_paths_walker(const bb_route_t *route, void *ctx)
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

// Builds the complete `{"oneOf":[{"$ref":"..."},...]}` fragment for every
// registered schema with a non-NULL sse_topic. Returns true and leaves a
// NUL-terminated fragment in out on success; returns false (out left
// unspecified) if the fragment would not fit — caller must not splice out.
//
// Every "did this snprintf fit" check below is a single unsigned comparison
// rather than `ret < 0 || (size_t)ret >= size`: snprintf only returns
// negative on an output/encoding error, which cannot happen writing plain
// ASCII into these buffers, so a separate `ret < 0` branch would be dead —
// untestable without faking libc — and would sit as a permanently-missed
// branch on every coverage run. Casting a hypothetical negative return to
// size_t wraps it to a huge value, which trivially satisfies `>= size`
// anyway, so the single comparison is both correct and branch-free for
// that case (nothing to exclude because there is no separate edge).
static bool build_sse_oneof_fragment(char *out, size_t out_size)
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
// Test-only seam: exposes the file-static fragment builder so host tests can
// drive it directly with a caller-controlled out_size (the cheapest lever
// for hitting its overflow branches — see test_sse_schema_fidelity.c), same
// "_for_test" + BB_<COMPONENT>_TESTING convention used elsewhere in this
// codebase (e.g. bb_ota_check_config_desc_for_test()).
bool bb_openapi_build_sse_oneof_fragment_for_test(char *out, size_t out_size)
{
    return build_sse_oneof_fragment(out, out_size);
}
#endif /* BB_OPENAPI_TESTING */

// ---------------------------------------------------------------------------
// Build a single operation object for a route
// ---------------------------------------------------------------------------

static bb_json_t build_operation(const bb_route_t *route)
{
    bb_json_t op = bb_json_obj_new();
    if (!op) return NULL;

    // operationId
    if (route->operation_id) {
        bb_json_obj_set_string(op, "operationId", route->operation_id);
    } else {
        char op_id[128];
        derive_operation_id(route->method, route->path, op_id, sizeof(op_id));
        bb_json_obj_set_string(op, "operationId", op_id);
    }

    // summary
    if (route->summary) {
        bb_json_obj_set_string(op, "summary", route->summary);
    }

    // tags (single-element array)
    if (route->tag) {
        bb_json_t tags = bb_json_arr_new();
        if (tags) {
            bb_json_arr_append_string(tags, route->tag);
            bb_json_obj_set_arr(op, "tags", tags);
        }
    }

    // parameters array (query / path / header)
    if (route->parameters && route->parameters_count > 0) {
        bb_json_t params = bb_json_arr_new();
        if (params) {
            for (size_t i = 0; i < route->parameters_count; i++) {
                const bb_route_param_t *p = &route->parameters[i];
                bb_json_t param_obj = bb_json_obj_new();
                if (!param_obj) continue;
                bb_json_obj_set_string(param_obj, "name", p->name ? p->name : "");
                bb_json_obj_set_string(param_obj, "in",   p->in   ? p->in   : "query");
                if (p->description) {
                    bb_json_obj_set_string(param_obj, "description", p->description);
                }
                bb_json_obj_set_bool(param_obj, "required", p->required);
                if (p->schema_type) {
                    bb_json_t schema = bb_json_obj_new();
                    if (schema) {
                        bb_json_obj_set_string(schema, "type", p->schema_type);
                        bb_json_obj_set_obj(param_obj, "schema", schema);
                    }
                }
                bb_json_arr_append_obj(params, param_obj);
            }
            bb_json_obj_set_arr(op, "parameters", params);
        }
    }

    // requestBody — gated on request_schema; request_content_type without schema is ignored
    if (route->request_schema) {
        bb_json_t req_body = bb_json_obj_new();
        bb_json_t content  = bb_json_obj_new();
        bb_json_t media    = bb_json_obj_new();

        if (req_body && content && media) {
            bb_json_obj_set_raw(media, "schema", route->request_schema);
            const char *ct = route->request_content_type
                             ? route->request_content_type
                             : "application/json";
            bb_json_obj_set_obj(content, ct, media);
            bb_json_obj_set_obj(req_body, "content", content);
            bb_json_obj_set_bool(req_body, "required", true);
            bb_json_obj_set_obj(op, "requestBody", req_body);
        } else {
            bb_json_free(req_body);
            bb_json_free(content);
            bb_json_free(media);
        }
    }

    // responses
    bb_json_t responses = bb_json_obj_new();
    if (responses && route->responses) {
        for (const bb_route_response_t *r = route->responses; r->status != 0; r++) {
            char status_key[8];
            snprintf(status_key, sizeof(status_key), "%d", r->status);

            bb_json_t resp_obj = bb_json_obj_new();
            if (!resp_obj) continue;

            // OpenAPI requires response.description; emit empty string when absent.
            bb_json_obj_set_string(resp_obj, "description",
                                   r->description ? r->description : "");

            if (r->schema) {
                bb_json_t content = bb_json_obj_new();
                bb_json_t media   = bb_json_obj_new();
                if (content && media) {
                    bb_json_obj_set_raw(media, "schema", r->schema);
                    const char *ct = r->content_type ? r->content_type : "application/json";
                    bb_json_obj_set_obj(content, ct, media);
                    bb_json_obj_set_obj(resp_obj, "content", content);
                } else {
                    bb_json_free(content);
                    bb_json_free(media);
                }
            } else if (r->content_type &&
                       strcmp(r->content_type, "text/event-stream") == 0) {
                // SSE route with no schema: synthesize oneOf from registered SSE
                // topics. The envelope is already 7 levels deep by the time
                // "schema" is reached (paths>path-item>operation>responses>
                // status>content>media); a nested bb_json oneOf array plus a
                // $ref object per entry would push past bb_http_resp_json_obj's
                // depth cap once PR4 ports this emitter onto that API. Build the
                // fragment as a bounded string instead and splice it in one shot
                // — the same mechanism already used for static schema literals.
                size_t sse_count = 0;
                for (size_t k = 0; k < s_schema_count; k++) {
                    if (s_schema_registry[k].sse_topic) sse_count++;
                }
                if (sse_count > 0) {
                    bb_json_t content = bb_json_obj_new();
                    bb_json_t media   = bb_json_obj_new();
                    if (content && media) {
                        // static, not stack: ~1.9KB is too large a fraction of
                        // the httpd worker's CONFIG_BB_HTTP_TASK_STACK_SIZE
                        // (default 6144, bb_http.c) to carry as an automatic
                        // array in this frame. Safe because esp_http_server
                        // runs exactly one FreeRTOS task per httpd_start()
                        // (bb_http.c holds a single s_server, started once)
                        // and that task serializes all request handling via
                        // select() — build_operation() is never reentered
                        // before this buffer's contents are consumed below.
                        // Gone entirely once PR4 streams the fragment directly.
                        static char oneof_buf[BB_OPENAPI_SSE_ONEOF_BUF_SIZE];
                        if (build_sse_oneof_fragment(oneof_buf, sizeof(oneof_buf))) {
                            bb_json_obj_set_raw(media, "schema", oneof_buf);
                            bb_json_obj_set_obj(content, "text/event-stream", media);
                            bb_json_obj_set_obj(resp_obj, "content", content);
                        } else {
                            // Fragment would not fit the bounded buffer — omit
                            // the content block rather than emit truncated
                            // (malformed) JSON, matching the sse_count == 0 path.
                            bb_log_e(TAG, "SSE oneOf fragment overflow — omitting content");
                            bb_json_free(content);
                            bb_json_free(media);
                        }
                    } else {
                        bb_json_free(content);
                        bb_json_free(media);
                    }
                }
            }

            bb_json_obj_set_obj(responses, status_key, resp_obj);
        }
    }

    if (responses) {
        bb_json_obj_set_obj(op, "responses", responses);
    }

    return op;
}

// Pass 2 context: emit operations for a specific path
typedef struct {
    const char *path;
    bb_json_t   path_item;
} pass2_ctx_t;

static void emit_operations_walker(const bb_route_t *route, void *ctx)
{
    pass2_ctx_t *p2 = (pass2_ctx_t *)ctx;
    if (strcmp(route->path, p2->path) != 0) return;

    bb_json_t op = build_operation(route);
    if (!op) return;

    bb_json_obj_set_obj(p2->path_item, method_str(route->method), op);
}

// ---------------------------------------------------------------------------
// Public emitter
// ---------------------------------------------------------------------------

bb_json_t bb_openapi_emit(const bb_openapi_meta_t *meta)
{
    if (!meta) {
        bb_log_e(TAG, "bb_openapi_emit: meta is NULL");
        return NULL;
    }

    bb_json_t root = bb_json_obj_new();
    if (!root) return NULL;

    // openapi version
    bb_json_obj_set_string(root, "openapi", "3.1.0");

    // info object
    bb_json_t info = bb_json_obj_new();
    if (!info) { bb_json_free(root); return NULL; }
    bb_json_obj_set_string(info, "title",   meta->title   ? meta->title   : "breadboard device");
    bb_json_obj_set_string(info, "version", meta->version ? meta->version : "0.0.0");
    if (meta->description) {
        bb_json_obj_set_string(info, "description", meta->description);
    }
    bb_json_obj_set_obj(root, "info", info);

    // servers (optional)
    if (meta->server_url) {
        bb_json_t servers  = bb_json_arr_new();
        bb_json_t server_e = bb_json_obj_new();
        if (servers && server_e) {
            bb_json_obj_set_string(server_e, "url", meta->server_url);
            bb_json_arr_append_obj(servers, server_e);
            bb_json_obj_set_arr(root, "servers", servers);
        } else {
            if (servers)  bb_json_free(servers);
            if (server_e) bb_json_free(server_e);
        }
    }

    // paths — two-pass
    path_set_t ps;
    memset(&ps, 0, sizeof(ps));
    bb_http_route_registry_foreach(collect_paths_walker, &ps);

    bb_json_t paths_obj = bb_json_obj_new();
    if (!paths_obj) { bb_json_free(root); return NULL; }

    for (size_t i = 0; i < ps.count; i++) {
        bb_json_t path_item = bb_json_obj_new();
        if (!path_item) continue;

        pass2_ctx_t p2 = { .path = ps.paths[i], .path_item = path_item };
        bb_http_route_registry_foreach(emit_operations_walker, &p2);

        bb_json_obj_set_obj(paths_obj, ps.paths[i], path_item);
    }

    bb_json_obj_set_obj(root, "paths", paths_obj);

    // components/schemas section (if registry non-empty)
    if (s_schema_count > 0) {
        bb_json_t components = bb_json_obj_new();
        bb_json_t schemas    = bb_json_obj_new();
        if (components && schemas) {
            for (size_t i = 0; i < s_schema_count; i++) {
                bb_json_obj_set_raw(schemas,
                                    s_schema_registry[i].component_name,
                                    s_schema_registry[i].schema_literal);
            }
            bb_json_obj_set_obj(components, "schemas", schemas);
            bb_json_obj_set_obj(root, "components", components);
        } else {
            bb_json_free(components);
            bb_json_free(schemas);
        }
    }

    return root;
}

// ---------------------------------------------------------------------------
// Streaming emitter — writes the OpenAPI document field-by-field directly
// via bb_http_resp_json_obj_* (bb_http_server's streaming JSON object
// primitive), never through bb_json / a bb_json_t tree. Peak memory is
// bounded to that primitive's ~1KB internal buffer plus whichever single
// schema literal is being spliced, instead of the heap+stack pressure of
// materializing the full document (bb_openapi_emit(), the tree path above,
// crashed httpd workers on tight-stack boards once route count grew past
// ~50 — B1-222).
//
// emit_operation() mirrors build_operation() field-for-field and must be
// kept in sync with it manually — test_openapi_emit_equivalence.c is the
// structural lock that catches drift between the two.
// ---------------------------------------------------------------------------

static void emit_operation(bb_http_json_obj_stream_t *s, const bb_route_t *route)
{
    // operationId
    if (route->operation_id) {
        bb_http_resp_json_obj_set_str(s, "operationId", route->operation_id);
    } else {
        char op_id[128];
        derive_operation_id(route->method, route->path, op_id, sizeof(op_id));
        bb_http_resp_json_obj_set_str(s, "operationId", op_id);
    }

    // summary
    if (route->summary) {
        bb_http_resp_json_obj_set_str(s, "summary", route->summary);
    }

    // tags (single-element array)
    if (route->tag) {
        bb_http_resp_json_obj_set_arr_begin(s, "tags");
        bb_http_resp_json_obj_set_str(s, NULL, route->tag);
        bb_http_resp_json_obj_set_arr_end(s);
    }

    // parameters array (query / path / header)
    if (route->parameters && route->parameters_count > 0) {
        bb_http_resp_json_obj_set_arr_begin(s, "parameters");
        for (size_t i = 0; i < route->parameters_count; i++) {
            const bb_route_param_t *p = &route->parameters[i];
            bb_http_resp_json_obj_set_obj_begin(s, NULL);  // array element: no key
            bb_http_resp_json_obj_set_str(s, "name", p->name ? p->name : "");
            bb_http_resp_json_obj_set_str(s, "in",   p->in   ? p->in   : "query");
            if (p->description) {
                bb_http_resp_json_obj_set_str(s, "description", p->description);
            }
            bb_http_resp_json_obj_set_bool(s, "required", p->required);
            if (p->schema_type) {
                bb_http_resp_json_obj_set_obj_begin(s, "schema");
                bb_http_resp_json_obj_set_str(s, "type", p->schema_type);
                bb_http_resp_json_obj_set_obj_end(s);
            }
            bb_http_resp_json_obj_set_obj_end(s);
        }
        bb_http_resp_json_obj_set_arr_end(s);
    }

    // requestBody — gated on request_schema; request_content_type without schema is ignored
    if (route->request_schema) {
        bb_http_resp_json_obj_set_obj_begin(s, "requestBody");
        bb_http_resp_json_obj_set_obj_begin(s, "content");
        const char *ct = route->request_content_type
                         ? route->request_content_type
                         : "application/json";
        bb_http_resp_json_obj_set_obj_begin(s, ct);
        // request_schema is a non-NULL JSON Schema literal by route-table
        // convention (never ""); _set_raw rejects a zero-length raw value.
        bb_http_resp_json_obj_set_raw(s, "schema", route->request_schema,
                                      strlen(route->request_schema));
        bb_http_resp_json_obj_set_obj_end(s);  // media
        bb_http_resp_json_obj_set_obj_end(s);  // content
        bb_http_resp_json_obj_set_bool(s, "required", true);
        bb_http_resp_json_obj_set_obj_end(s);  // requestBody
    }

    // responses
    bb_http_resp_json_obj_set_obj_begin(s, "responses");
    if (route->responses) {
        for (const bb_route_response_t *r = route->responses; r->status != 0; r++) {
            char status_key[8];
            snprintf(status_key, sizeof(status_key), "%d", r->status);

            bb_http_resp_json_obj_set_obj_begin(s, status_key);

            // OpenAPI requires response.description; emit empty string when absent.
            bb_http_resp_json_obj_set_str(s, "description",
                                          r->description ? r->description : "");

            if (r->schema) {
                bb_http_resp_json_obj_set_obj_begin(s, "content");
                const char *ct = r->content_type ? r->content_type : "application/json";
                bb_http_resp_json_obj_set_obj_begin(s, ct);
                // r->schema is a non-NULL JSON Schema literal by route-table
                // convention (never ""); _set_raw rejects a zero-length raw value.
                bb_http_resp_json_obj_set_raw(s, "schema", r->schema, strlen(r->schema));
                bb_http_resp_json_obj_set_obj_end(s);  // media
                bb_http_resp_json_obj_set_obj_end(s);  // content
            } else if (r->content_type &&
                       strcmp(r->content_type, "text/event-stream") == 0) {
                // SSE route with no schema: synthesize oneOf from registered SSE
                // topics, spliced as one bounded raw fragment — see
                // build_sse_oneof_fragment()'s doc comment for why this can't
                // be a nested object here (depth-cap headroom).
                size_t sse_count = 0;
                for (size_t k = 0; k < s_schema_count; k++) {
                    if (s_schema_registry[k].sse_topic) sse_count++;
                }
                if (sse_count > 0) {
                    static char oneof_buf[BB_OPENAPI_SSE_ONEOF_BUF_SIZE];
                    if (build_sse_oneof_fragment(oneof_buf, sizeof(oneof_buf))) {
                        bb_http_resp_json_obj_set_obj_begin(s, "content");
                        bb_http_resp_json_obj_set_obj_begin(s, "text/event-stream");
                        bb_http_resp_json_obj_set_raw(s, "schema", oneof_buf, strlen(oneof_buf));
                        bb_http_resp_json_obj_set_obj_end(s);  // media
                        bb_http_resp_json_obj_set_obj_end(s);  // content
                    } else {
                        // Fragment would not fit the bounded buffer — omit the
                        // content block rather than emit truncated JSON,
                        // matching the sse_count == 0 path.
                        bb_log_e(TAG, "SSE oneOf fragment overflow — omitting content");
                    }
                }
            }

            bb_http_resp_json_obj_set_obj_end(s);  // status
        }
    }
    bb_http_resp_json_obj_set_obj_end(s);  // responses
}

// Pass-2 walker: emits one method's operation object into the currently-open
// path-item object.
typedef struct {
    const char                *path;
    bb_http_json_obj_stream_t *stream;
} stream_path_ctx_t;

static void stream_operations_walker(const bb_route_t *route, void *ctx)
{
    stream_path_ctx_t *pc = (stream_path_ctx_t *)ctx;
    if (strcmp(route->path, pc->path) != 0) return;

    bb_http_resp_json_obj_set_obj_begin(pc->stream, method_str(route->method));
    emit_operation(pc->stream, route);
    bb_http_resp_json_obj_set_obj_end(pc->stream);
}

bb_err_t bb_openapi_emit_stream(bb_http_request_t *req,
                                const bb_openapi_meta_t *meta)
{
    if (!req || !meta) return BB_ERR_INVALID_ARG;

    bb_openapi_meta_t effective = *meta;
    if (!effective.title)   effective.title   = "breadboard device";
    if (!effective.version) effective.version = "0.0.0";

    bb_http_json_obj_stream_t s;
    bb_err_t err = bb_http_resp_json_obj_begin(req, &s);
    if (err != BB_OK) return err;

    bb_http_resp_json_obj_set_str(&s, "openapi", "3.1.0");

    // info
    bb_http_resp_json_obj_set_obj_begin(&s, "info");
    bb_http_resp_json_obj_set_str(&s, "title",   effective.title);
    bb_http_resp_json_obj_set_str(&s, "version", effective.version);
    if (effective.description) {
        bb_http_resp_json_obj_set_str(&s, "description", effective.description);
    }
    bb_http_resp_json_obj_set_obj_end(&s);

    // servers — optional, single-element array.
    if (effective.server_url) {
        bb_http_resp_json_obj_set_arr_begin(&s, "servers");
        bb_http_resp_json_obj_set_obj_begin(&s, NULL);  // array element: no key
        bb_http_resp_json_obj_set_str(&s, "url", effective.server_url);
        bb_http_resp_json_obj_set_obj_end(&s);
        bb_http_resp_json_obj_set_arr_end(&s);
    }

    // paths — two-pass, same as bb_openapi_emit() above.
    path_set_t ps;
    memset(&ps, 0, sizeof(ps));
    bb_http_route_registry_foreach(collect_paths_walker, &ps);

    bb_http_resp_json_obj_set_obj_begin(&s, "paths");
    for (size_t i = 0; i < ps.count; i++) {
        // Paths in the bb_http registry are static const char * literals
        // composed of /[a-zA-Z0-9_/-]+/ — JSON-safe without escaping, and used
        // directly as the key here (no risk of colliding with a NULL key).
        bb_http_resp_json_obj_set_obj_begin(&s, ps.paths[i]);
        stream_path_ctx_t pc = { .path = ps.paths[i], .stream = &s };
        bb_http_route_registry_foreach(stream_operations_walker, &pc);
        bb_http_resp_json_obj_set_obj_end(&s);
    }
    bb_http_resp_json_obj_set_obj_end(&s);  // paths

    // components/schemas section (if registry non-empty)
    if (s_schema_count > 0) {
        bb_http_resp_json_obj_set_obj_begin(&s, "components");
        bb_http_resp_json_obj_set_obj_begin(&s, "schemas");
        for (size_t i = 0; i < s_schema_count; i++) {
            bb_http_resp_json_obj_set_raw(&s, s_schema_registry[i].component_name,
                                          s_schema_registry[i].schema_literal,
                                          strlen(s_schema_registry[i].schema_literal));
        }
        bb_http_resp_json_obj_set_obj_end(&s);  // schemas
        bb_http_resp_json_obj_set_obj_end(&s);  // components
    }

    return bb_http_resp_json_obj_end(&s);
}
