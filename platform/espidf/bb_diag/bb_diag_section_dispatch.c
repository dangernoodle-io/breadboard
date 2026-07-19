// ESP-IDF dispatcher for the bb_diag section registry (B1-diag-dissolution
// PR3). Thin glue only -- name parsing and query-param threading are pure,
// portable helpers in components/bb_diag/bb_diag_section.c (see
// bb_diag_section_priv.h), exercised directly by host tests. This file's
// own job is exactly two things: pull request-scoped values out of
// bb_http_request_t (bb_http_req_uri/bb_http_req_query_key_value) and drive
// the render+respond path, same common shape as floor_app.c's
// floor_diag_render() (examples/floor/main/floor_app.c ~L124-164).
#include "bb_diag_section_priv.h"

#include "bb_http_serialize_stream.h"
#include "bb_http_server.h"
#include "bb_log.h"
#include "bb_mem.h"

#include <stdint.h>
#include <string.h>

static const char *TAG = "bb_diag_section_dispatch";

// bb_diag_query_getter_fn adapter over bb_http_req_query_key_value() --
// decouples bb_diag_section_build_query() (portable, host-tested directly
// against a fake getter) from bb_http_request_t.
static bool http_query_getter(void *ctx, const char *key, char *out, size_t out_cap)
{
    bb_http_request_t *req = (bb_http_request_t *)ctx;
    return bb_http_req_query_key_value(req, key, out, out_cap) == BB_OK;
}

// Sends a JSON error body with Content-Type + status set on both the
// success and error paths (bb_http_resp_sendstr() itself does not set
// Content-Type) -- same convention as floor_diag_render().
static bb_err_t respond_error(bb_http_request_t *req, int status, const char *body)
{
    bb_http_resp_set_type(req, "application/json");
    bb_http_resp_set_status(req, status);
    return bb_http_resp_sendstr(req, body);
}

static bb_err_t diag_section_dispatch(bb_http_request_t *req)
{
    char uri[BB_DIAG_SECTION_URI_MAX];
    bb_err_t rc = bb_http_req_uri(req, uri, sizeof(uri));
    if (rc != BB_OK) {
        return respond_error(req, 404, "{\"error\":\"not found\"}");
    }

    char name[BB_DIAG_SECTION_NAME_MAX];
    rc = bb_diag_section_name_from_uri(uri, name, sizeof(name));
    if (rc != BB_OK) {
        return respond_error(req, 404, "{\"error\":\"not found\"}");
    }

    const bb_diag_section_t *sec = bb_diag_section_find(name);
    if (!sec) {
        return respond_error(req, 404, "{\"error\":\"not found\"}");
    }

    bb_serialize_query_t query = {0};
    char value_scratch[BB_SERIALIZE_QUERY_MAX_PARAMS][BB_DIAG_SECTION_QUERY_VALUE_BYTES];
    rc = bb_diag_section_build_query(sec, http_query_getter, req, (char *)value_scratch, &query);
    if (rc != BB_OK) {  // LCOV_EXCL_BR_LINE -- bb_diag_section_build_query() only returns non-BB_OK on NULL args, none of which are ever NULL at this call site (sec/http_query_getter/req/value_scratch/&query are all always valid here).
        return respond_error(req, 500, "{\"error\":\"query build failed\"}");
    }

    bb_diag_fill_args_t fill_args = {
        .ctx   = sec->ctx,
        .query = sec->n_query_keys > 0 ? &query : NULL,
    };

    if (sec->iter) {
        // Two-phase, arena-owned-by-the-dispatcher iter path (see
        // bb_diag_section.h's bb_diag_iter_fn doc). No defensive row-count
        // backstop knob -- bb_malloc_prefer_spiram() already fails closed
        // (NULL -> 500 below, before any chunk reaches the wire).
        char dst[BB_DIAG_SECTION_SCRATCH_BYTES];
        size_t row_count = 0;
        rc = sec->iter(dst, NULL, 0, &row_count, &fill_args);  // phase 1: count
        if (rc != BB_OK) {
            return respond_error(req, 500, "{\"error\":\"fill failed\"}");
        }

        void *arena = NULL;
        if (row_count > 0) {
            size_t elem_size = bb_diag_section_stream_elem_size(sec);
            // Loud reject over a silent size_t multiply wrap -- same
            // philosophy as the rest of this PR (no defensive row-count
            // cap, but never trust an unchecked multiply into an
            // allocation size).
            if (elem_size != 0 && row_count > SIZE_MAX / elem_size) {
                return respond_error(req, 500, "{\"error\":\"row count overflow\"}");
            }
            arena = bb_malloc_prefer_spiram(row_count * elem_size);
            if (!arena) {
                return respond_error(req, 500, "{\"error\":\"arena alloc failed\"}");
            }
        }

        rc = sec->iter(dst, arena, row_count, &row_count, &fill_args);  // phase 2: fill
        if (rc != BB_OK) {
            bb_mem_free(arena);
            return respond_error(req, 500, "{\"error\":\"fill failed\"}");
        }

        rc = bb_http_serialize_stream(req, sec->snap_desc, dst);
        bb_mem_free(arena);
        return rc;
    }

    char scratch[BB_DIAG_SECTION_SCRATCH_BYTES];
    rc = sec->fill(scratch, &fill_args);
    if (rc != BB_OK) {
        return respond_error(req, 500, "{\"error\":\"fill failed\"}");
    }

    return bb_http_serialize_stream(req, sec->snap_desc, scratch);
}

bb_err_t bb_diag_sections_init(bb_http_handle_t server)
{
    bb_err_t rc = bb_http_register_route(server, BB_HTTP_GET, "/api/diag/*", diag_section_dispatch);
    if (rc != BB_OK) {
        bb_log_e(TAG, "failed to register /api/diag/* wildcard route: %d", (int)rc);
        return rc;
    }

    bb_log_i(TAG, "diag section wildcard route registered");
    return BB_OK;
}
