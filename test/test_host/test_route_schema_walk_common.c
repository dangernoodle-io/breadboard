#include "test_route_schema_walk_common.h"

#include <stdio.h>

#include "cJSON.h"

void test_route_schema_walk_check_one(const char *route, const char *which,
                                       const char *schema,
                                       test_route_schema_walk_ctx_t *ctx)
{
    if (!schema) return;
    ctx->schema_count++;
    cJSON *parsed = cJSON_Parse(schema);
    if (!parsed) {
        if (ctx->failures == 0) {
            const char *err = cJSON_GetErrorPtr();
            long off = err ? (long)(err - schema) : -1;
            snprintf(ctx->first_failure, sizeof(ctx->first_failure),
                     "%s [%s]: malformed JSON at offset %ld",
                     route ? route : "(null)", which, off);
        }
        ctx->failures++;
        return;
    }
    cJSON_Delete(parsed);
}

void test_route_schema_walk(const bb_route_t *route, void *ctx_)
{
    test_route_schema_walk_ctx_t *ctx = (test_route_schema_walk_ctx_t *)ctx_;
    if (!route) return;
    ctx->route_count++;
    test_route_schema_walk_check_one(route->path, "request", route->request_schema, ctx);
    if (route->responses) {
        for (const bb_route_response_t *r = route->responses; r->status != 0; r++) {
            test_route_schema_walk_check_one(route->path, "response", r->schema, ctx);
        }
    }
}
