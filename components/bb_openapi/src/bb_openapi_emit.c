#include "bb_openapi.h"
#include "bb_openapi_emit_internal.h"
#include "bb_log.h"

#include <stdbool.h>
#include <string.h>
#include <stdio.h>

static const char *TAG = "bb_openapi_emit";

// ---------------------------------------------------------------------------
// Streaming emitter — writes the OpenAPI document field-by-field directly
// via bb_http_resp_json_obj_* (bb_http_server's streaming JSON object
// primitive), never through bb_json / a bb_json_t tree. Peak memory is
// bounded to that primitive's ~1KB internal buffer plus whichever single
// schema literal is being spliced, instead of the heap+stack pressure of
// materializing the full document — an earlier in-memory tree builder,
// bb_openapi_emit(), crashed httpd workers on tight-stack boards once route
// count grew past ~50 (B1-222); it was removed in B1-1054 once this
// streaming path reached full parity.
//
// emit_operation() is the sole operationId/schema serializer now that the
// host tree emitter is gone — no counterpart to stay in sync with. Shared
// helpers (method_str, derive_operation_id, path_set_*, collect_paths_walker,
// build_sse_oneof_fragment) still live in bb_openapi_emit_shared.c (see
// bb_openapi_emit_internal.h), kept separate from this file for test-only
// seam access rather than for sharing with a second emitter.
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
                if (sse_schema_count() > 0) {
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

    // paths — two-pass: collect unique paths first, then emit each path's
    // operations, since the OpenAPI paths object groups all methods under
    // one path key rather than listing routes flat.
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
    size_t schema_count = bb_openapi_schema_count();
    if (schema_count > 0) {
        bb_http_resp_json_obj_set_obj_begin(&s, "components");
        bb_http_resp_json_obj_set_obj_begin(&s, "schemas");
        for (size_t i = 0; i < schema_count; i++) {
            bb_openapi_schema_entry_t entry;
            // i < schema_count (== bb_openapi_schema_count() above) always
            // satisfies bb_openapi_schema_get()'s bounds check; the false
            // path is unreachable from this loop, so the return value is
            // intentionally not checked here (checking it would add a
            // permanently-missed branch — see sse_schema_count()'s comment
            // in bb_openapi_emit_internal.h for the same reasoning).
            bb_openapi_schema_get(i, &entry);
            bb_http_resp_json_obj_set_raw(&s, entry.component_name,
                                          entry.schema_literal,
                                          strlen(entry.schema_literal));
        }
        bb_http_resp_json_obj_set_obj_end(&s);  // schemas
        bb_http_resp_json_obj_set_obj_end(&s);  // components
    }

    return bb_http_resp_json_obj_end(&s);
}
