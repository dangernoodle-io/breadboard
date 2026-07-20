// ESP-IDF dispatcher for the bb_http_section namespace registry
// (bb_http_section PR). Thin glue only -- namespace lookup/name-strip is a
// pure, portable helper in components/bb_http_server/src/bb_http_section.c
// (see bb_http_section_priv.h), and the err->status decision is a pure
// helper in components/bb_http_server/src/bb_http_section_status.c -- both
// exercised directly by host tests. This file's own job is exactly two
// things: pull request-scoped values out of bb_http_request_t
// (bb_http_req_uri/bb_http_req_recv_body_stack) and drive the
// find-namespace -> method-branch -> respond path, same shape as
// bb_diag_section_dispatch.c's own GET-only dispatcher.
//
// QUERY THREADING: deferred. Every namespace's render() hook is called with
// `query == NULL` here -- no real consumer needs request-scoped query
// filtering yet (this PR ships the mechanism only; see bb_http_section.h's
// own "MECHANISM ONLY" note). A future consumer that needs it can extend
// this dispatcher the same way bb_diag_section_dispatch.c threads its own
// declared query_keys, without changing bb_http_section_render_fn's
// signature (it already carries a `query` parameter).
#include "bb_http_section_priv.h"
#include "bb_http_section_status.h"

#include "bb_http_body.h"
#include "bb_http_server.h"
#include "bb_log.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "bb_http_section_dispatch";

// Request body cap for a PATCH/POST apply -- generous enough for any
// section's ingress document, small enough to stay a stack buffer (mirrors
// bb_wifi_http_routes.c's/bb_storage_http_routes.c's own per-route body
// caps). Sections needing a larger document register their own route
// instead of this shared dispatcher. This is the MAX BODY BYTES the buffer
// can hold (see bb_http_req_recv_body_stack()'s cap-semantics doc) -- the
// stack buffer itself is sized BB_HTTP_SECTION_BODY_MAX + 1.
#define BB_HTTP_SECTION_BODY_MAX 512

// The JSON-error-body idiom this used to hand-roll as a local respond_error()
// (verbatim duplicate of bb_diag_section_dispatch.c's own respond_error())
// is now the single shared bb_http_send_json_error() in bb_http_server.h.

static bb_err_t section_get_handler(bb_http_request_t *req)
{
    char uri[BB_HTTP_SECTION_URI_MAX];
    bb_err_t rc = bb_http_req_uri(req, uri, sizeof(uri));
    if (rc != BB_OK) {
        return bb_http_send_json_error(req, 404, "{\"error\":\"not found\"}");
    }

    char name[BB_HTTP_SECTION_NAME_MAX];
    const bb_http_section_ns_t *ns = bb_http_section_find(uri, name, sizeof(name));
    if (!ns) {
        return bb_http_send_json_error(req, 404, "{\"error\":\"not found\"}");
    }
    if (!ns->render) {
        return bb_http_send_json_error(req, 405, "{\"error\":\"method not allowed\"}");
    }

    char   buf[BB_HTTP_SECTION_BODY_MAX];
    size_t out_len = 0;
    rc = ns->render(name, NULL, buf, sizeof(buf), &out_len, ns->ctx);

    int status = bb_http_section_status_for_render(rc);
    if (status != 200) {
        char body[64];
        snprintf(body, sizeof(body), "{\"error\":\"render failed\",\"rc\":%d}", (int)rc);
        return bb_http_send_json_error(req, status, body);
    }

    if (out_len >= sizeof(buf)) out_len = sizeof(buf) - 1;  // defensive; render() itself owns cap
    buf[out_len] = '\0';
    bb_http_resp_set_status(req, 200);
    return bb_http_resp_sendstr(req, buf);
}

static bb_err_t section_patch_handler(bb_http_request_t *req)
{
    char uri[BB_HTTP_SECTION_URI_MAX];
    bb_err_t rc = bb_http_req_uri(req, uri, sizeof(uri));
    if (rc != BB_OK) {
        return bb_http_send_json_error(req, 404, "{\"error\":\"not found\"}");
    }

    char name[BB_HTTP_SECTION_NAME_MAX];
    const bb_http_section_ns_t *ns = bb_http_section_find(uri, name, sizeof(name));
    if (!ns) {
        return bb_http_send_json_error(req, 404, "{\"error\":\"not found\"}");
    }
    if (!ns->apply) {
        return bb_http_send_json_error(req, 405, "{\"error\":\"method not allowed\"}");
    }

    char   body[BB_HTTP_SECTION_BODY_MAX + 1];
    size_t n = 0;
    if (bb_http_req_recv_body_stack(req, body, sizeof(body), &n) != BB_OK) {
        return bb_http_send_json_error(req, 400, "{\"error\":\"missing, oversized, or unreadable body\"}");
    }

    bb_http_section_apply_result_t result = ns->apply(name, body, n, ns->ctx);

    int status = bb_http_section_status_for_apply(result, ns->unsupported_status);
    if (status != 200) {
        char err_body[64];
        snprintf(err_body, sizeof(err_body), "{\"error\":\"apply failed\",\"rc\":%d}", (int)result.rc);
        return bb_http_send_json_error(req, status, err_body);
    }

    return bb_http_resp_no_content(req);
}

bb_err_t bb_http_section_init(bb_http_handle_t server)
{
    // One GET + one PATCH route per registered namespace's own
    // "<prefix>*" wildcard (NOT a single blanket "/api/*" -- that would
    // shadow every other /api/* consumer). Each still routes through the
    // existing software /api/* dispatch table (bb_dispatch_api_add()), so
    // this costs 0 additional httpd slots regardless of namespace count.
    size_t n = bb_http_section_count();
    for (size_t i = 0; i < n; i++) {
        const char *wildcard = NULL;
        const bb_http_section_ns_t *ns = bb_http_section_at(i, &wildcard);
        if (!ns) continue;  // LCOV_EXCL_LINE -- defensive; i < n by loop bound

        bb_err_t rc = bb_http_register_route(server, BB_HTTP_GET, wildcard, section_get_handler);
        if (rc != BB_OK) {
            bb_log_e(TAG, "failed to register section GET dispatch for %s: %d", wildcard, (int)rc);
            return rc;
        }

        rc = bb_http_register_route(server, BB_HTTP_PATCH, wildcard, section_patch_handler);
        if (rc != BB_OK) {
            bb_log_e(TAG, "failed to register section PATCH dispatch for %s: %d", wildcard, (int)rc);
            return rc;
        }
    }

    bb_log_i(TAG, "http section dispatch registered (%u namespace(s))", (unsigned)n);
    return BB_OK;
}
