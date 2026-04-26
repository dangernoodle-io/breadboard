#include "bb_openapi.h"
#include "bb_log.h"

#include <string.h>
#include <ctype.h>
#include <stdio.h>

static const char *TAG = "bb_openapi";

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
    bb_json_obj_set_string(info, "title",   meta->title   ? meta->title   : "");
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

    return root;
}
