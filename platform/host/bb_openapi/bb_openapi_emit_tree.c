// bb_openapi_emit() (B1-1116) — host-only tree emitter, relocated out of
// components/bb_openapi/src/bb_openapi_emit.c. Zero device callers: the
// device path is bb_openapi_emit_stream() (bb_openapi_emit.c), which never
// materializes a bb_json_t tree. This file IS the load-bearing entrypoint
// for host_tools/emit_openapi and ~380 host assertions across
// test_openapi_emit.c / test_sse_schema_fidelity.c / two integration tests,
// so it isn't deleted — filed under platform/host/ instead, mirroring
// bb_openapi_validate.c's precedent (see components/bb_openapi/CMakeLists.txt).
// Not built by idf_component_register; never linked into firmware, so its
// bb_json_t / cJSON dependency never ships in the image.
#include "bb_openapi.h"
#include "bb_openapi_emit_internal.h"
#include "bb_openapi_emit_tree_priv.h"
#include "bb_log.h"

#include <stdbool.h>
#include <string.h>
#include <stdio.h>

static const char *TAG = "bb_openapi_emit_tree";

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
                if (sse_schema_count() > 0) {
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
    size_t schema_count = bb_openapi_schema_count();
    if (schema_count > 0) {
        bb_json_t components = bb_json_obj_new();
        bb_json_t schemas    = bb_json_obj_new();
        if (components && schemas) {
            for (size_t i = 0; i < schema_count; i++) {
                bb_openapi_schema_entry_t entry;
                // i < schema_count (== bb_openapi_schema_count() above) always
                // satisfies bb_openapi_schema_get()'s bounds check; the false
                // path is unreachable from this loop, so the return value is
                // intentionally not checked here (checking it would add a
                // permanently-missed branch — see sse_schema_count()'s
                // comment in bb_openapi_emit_internal.h for the same
                // reasoning).
                bb_openapi_schema_get(i, &entry);
                bb_json_obj_set_raw(schemas, entry.component_name, entry.schema_literal);
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
