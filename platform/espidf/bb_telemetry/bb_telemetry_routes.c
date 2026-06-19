// bb_telemetry — GET + PATCH /api/telemetry and GET /api/telemetry/metrics routes.
//
// GET  /api/telemetry  builds {<name>: {fields}} per registered section.
// PATCH /api/telemetry dispatches parsed body sub-objects to section patch_fn.
//       Returns 400 if a present section is read-only.
//       Returns 204 on success.
// GET  /api/telemetry/metrics    enumerates bb_pub sources and emits Prometheus
//       exposition format (default) or JSON, with optional schema-only views
//       (B1-295). The metric name prefix comes from bb_pub_metrics_prefix().
#include "bb_telemetry.h"
#include "bb_http.h"
#include "bb_json.h"
#include "bb_log.h"
#include "bb_registry.h"
#include "bb_pub.h"
#include "bb_nv.h"
#include "bb_clock.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "bb_telemetry";

#define BB_TELEMETRY_BODY_MAX 8192

// ---------------------------------------------------------------------------
// Helper: send JSON error
// ---------------------------------------------------------------------------

static void send_json_error(bb_http_request_t *req, int status, const char *msg)
{
    bb_http_resp_set_status(req, status);
    bb_http_json_obj_stream_t obj;
    bb_http_resp_json_obj_begin(req, &obj);
    bb_http_resp_json_obj_set_str(&obj, "error", msg);
    bb_http_resp_json_obj_end(&obj);
}

// ---------------------------------------------------------------------------
// GET /api/telemetry
// ---------------------------------------------------------------------------

static bb_err_t telemetry_get_handler(bb_http_request_t *req)
{
    bb_json_t root = bb_json_obj_new();
    if (!root) {
        send_json_error(req, 500, "out of memory");
        return BB_ERR_NO_SPACE;
    }

    bb_telemetry_build_get(root);
    bb_json_obj_set_bool(root, "pending_reboot", bb_telemetry_pending_reboot());

    char *json = bb_json_serialize(root);
    bb_json_free(root);
    if (!json) {
        send_json_error(req, 500, "serialize failed");
        return BB_ERR_NO_SPACE;
    }

    bb_http_resp_set_type(req, "application/json");
    bb_http_resp_send_chunk(req, json, (int)strlen(json));
    bb_http_resp_send_chunk(req, NULL, 0);
    bb_json_free_str(json);
    return BB_OK;
}

// ---------------------------------------------------------------------------
// PATCH /api/telemetry
// ---------------------------------------------------------------------------

static bb_err_t telemetry_patch_handler(bb_http_request_t *req)
{
    int body_len = bb_http_req_body_len(req);
    if (body_len <= 0 || body_len > BB_TELEMETRY_BODY_MAX) {
        send_json_error(req, 400, "missing or oversized body");
        return BB_ERR_INVALID_ARG;
    }

    char *body = malloc((size_t)body_len + 1);
    if (!body) {
        send_json_error(req, 400, "out of memory");
        return BB_ERR_NO_SPACE;
    }

    int n = bb_http_req_recv(req, body, (size_t)(body_len + 1));
    if (n < 0) {
        free(body);
        send_json_error(req, 400, "read failed");
        return BB_ERR_INVALID_ARG;
    }
    body[n] = '\0';

    bb_json_t parsed = bb_json_parse(body, (size_t)n);
    free(body);
    if (!parsed) {
        send_json_error(req, 400, "invalid JSON");
        return BB_ERR_INVALID_ARG;
    }

    bb_err_t rc = bb_telemetry_dispatch_patch(parsed);
    bb_json_free(parsed);

    if (rc == BB_ERR_INVALID_ARG) {
        send_json_error(req, 400, "PATCH on read-only section");
        return rc;
    }
    if (rc == BB_ERR_CONFLICT) {
        send_json_error(req, 409,
                        "another telemetry sink is active; disable it first");
        return rc;
    }
    if (rc != BB_OK) {
        send_json_error(req, 500, "patch failed");
        return rc;
    }

    // B1-289: config persisted to NVS; reboot required to apply.
    bb_http_resp_set_status(req, 200);
    bb_http_json_obj_stream_t obj;
    bb_http_resp_json_obj_begin(req, &obj);
    bb_http_resp_json_obj_set_bool(&obj, "reboot_required", true);
    bb_http_resp_json_obj_end(&obj);
    return BB_OK;
}

// ---------------------------------------------------------------------------
// Route descriptors — GET responses[0].schema filled at init from registered sections
// ---------------------------------------------------------------------------

static bb_route_response_t s_telemetry_get_responses[] = {
    { 200, "application/json",
      NULL,  // filled by bb_telemetry_assemble_get_schema() at init
      "Telemetry sections (mqtt, http, publisher)" },
    { 0 },
};

static const bb_route_response_t s_telemetry_patch_responses[] = {
    { 200, "application/json",
      "{\"type\":\"object\","
      "\"properties\":{\"reboot_required\":{\"type\":\"boolean\"}},"
      "\"required\":[\"reboot_required\"]}",
      "settings persisted to NVS; reboot required to apply" },
    { 400, "application/json",
      "{\"type\":\"object\","
      "\"properties\":{\"error\":{\"type\":\"string\"}},"
      "\"required\":[\"error\"]}",
      "bad or missing request body, or read-only section" },
    { 409, "application/json",
      "{\"type\":\"object\","
      "\"properties\":{\"error\":{\"type\":\"string\"}},"
      "\"required\":[\"error\"]}",
      "another telemetry sink is already active" },
    { 0 },
};

static bb_route_t s_telemetry_get_route = {
    .method    = BB_HTTP_GET,
    .path      = "/api/telemetry",
    .tag       = "telemetry",
    .summary   = "Get telemetry configuration (mqtt, http, publisher sections)",
    .responses = s_telemetry_get_responses,
    .handler   = telemetry_get_handler,
};

static const bb_route_t s_telemetry_patch_route = {
    .method               = BB_HTTP_PATCH,
    .path                 = "/api/telemetry",
    .tag                  = "telemetry",
    .summary              = "Patch telemetry configuration sections",
    .request_content_type = "application/json",
    .request_schema       =
        "{\"type\":\"object\","
        "\"description\":\"Keys are section names (mqtt, http, publisher); "
                         "values are section-specific patch objects\"}",
    .responses = s_telemetry_patch_responses,
    .handler   = telemetry_patch_handler,
};

// ===========================================================================
// GET /api/telemetry/metrics — Prometheus-style telemetry endpoint (B1-295)
//
// Folded into bb_telemetry per decision #402: it shares bb_telemetry's exact
// dependency closure (bb_pub, bb_http, bb_json, bb_nv_config), so it is part
// of bb_telemetry rather than a separate component.
//
// Four combos driven by query params `format` and `schema`:
//   GET /api/telemetry/metrics                    -> Prometheus text, live values
//   GET /api/telemetry/metrics?format=json        -> JSON snapshot
//   GET /api/telemetry/metrics?schema             -> Prometheus TYPE/HELP lines, no values
//   GET /api/telemetry/metrics?schema&format=json -> JSON schema descriptor
// ===========================================================================

// ---------------------------------------------------------------------------
// Name sanitization — Prometheus: [a-zA-Z_:][a-zA-Z0-9_:]*
// ---------------------------------------------------------------------------

static void metrics_sanitize_name(const char *in, char *out, size_t out_size)
{
    size_t i = 0;
    size_t j = 0;
    while (in[i] && j + 1 < out_size) {
        char c = in[i++];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == ':') {
            out[j++] = c;
        } else {
            out[j++] = '_';
        }
    }
    out[j] = '\0';
}

// Build full metric name: <prefix>_<subtopic>_<field>
static void metrics_make_name(const char *prefix, const char *subtopic,
                              const char *field, char *out, size_t out_size)
{
    char tmp[128];
    snprintf(tmp, sizeof(tmp), "%s_%s_%s", prefix, subtopic, field);
    metrics_sanitize_name(tmp, out, out_size);
}

// ---------------------------------------------------------------------------
// Label-value escaping — \ → \\, " → \", \n → \n
// ---------------------------------------------------------------------------

static void metrics_escape_label_val(const char *in, char *out, size_t out_size)
{
    size_t j = 0;
    for (size_t i = 0; in[i] && j + 2 < out_size; i++) {
        char c = in[i];
        if (c == '\\') {
            out[j++] = '\\'; out[j++] = '\\';
        } else if (c == '"') {
            out[j++] = '\\'; out[j++] = '"';
        } else if (c == '\n') {
            out[j++] = '\\'; out[j++] = 'n';
        } else {
            out[j++] = c;
        }
    }
    out[j] = '\0';
}

// ---------------------------------------------------------------------------
// Send helper — write a string to the chunked response
// ---------------------------------------------------------------------------

static bb_err_t metrics_chunk(bb_http_request_t *req, const char *s)
{
    return bb_http_resp_send_chunk(req, s, -1);
}

// ---------------------------------------------------------------------------
// Walk context for Prometheus per source
// ---------------------------------------------------------------------------

typedef struct {
    bb_http_request_t *req;
    const char        *prefix;
    const char        *subtopic;
    const char        *host;
    bool               schema_only;   // prom: TYPE/HELP only
    bb_err_t           err;
    // For Prometheus: accumulate string fields into an _info line
    char               info_labels[512];
    bool               has_info;
} metrics_walk_ctx_t;

static void metrics_prom_walk_cb(const char *key, bb_json_t child, void *ctx_)
{
    metrics_walk_ctx_t *ctx = (metrics_walk_ctx_t *)ctx_;
    if (ctx->err != BB_OK) return;
    if (!key) return;  // array element — skip
    if (strcmp(key, "ts") == 0 || strcmp(key, "transport") == 0 ||
        strcmp(key, "tls") == 0) return;  // skip injected fields

    char name[128];
    metrics_make_name(ctx->prefix, ctx->subtopic, key, name, sizeof(name));

    char host_escaped[128];
    metrics_escape_label_val(ctx->host ? ctx->host : "", host_escaped, sizeof(host_escaped));

    if (bb_json_item_is_number(child) || bb_json_item_is_true(child) ||
        (!bb_json_item_is_string(child) && !bb_json_item_is_null(child) &&
         !bb_json_item_is_array(child) && !bb_json_item_is_object(child))) {
        // Numeric / bool → gauge
        char line[256];
        snprintf(line, sizeof(line), "# TYPE %s gauge\n", name);
        if (metrics_chunk(ctx->req, line) != BB_OK) { ctx->err = BB_ERR_INVALID_STATE; return; }
        if (!ctx->schema_only) {
            double val = bb_json_item_is_true(child) ? 1.0 : bb_json_item_get_double(child);
            snprintf(line, sizeof(line), "%s{host=\"%s\"} %.6g\n",
                     name, host_escaped, val);
            if (metrics_chunk(ctx->req, line) != BB_OK) { ctx->err = BB_ERR_INVALID_STATE; return; }
        }
    } else if (bb_json_item_is_string(child)) {
        const char *sval = bb_json_item_get_string(child);
        char escaped_val[256];
        metrics_escape_label_val(sval ? sval : "", escaped_val, sizeof(escaped_val));
        char label_frag[320];
        char label_key[64];
        metrics_sanitize_name(key, label_key, sizeof(label_key));
        snprintf(label_frag, sizeof(label_frag), ",%s=\"%s\"", label_key, escaped_val);
        size_t cur = strlen(ctx->info_labels);
        size_t avail = sizeof(ctx->info_labels) - cur;
        if (avail > 1) {
            strncat(ctx->info_labels, label_frag, avail - 1);
        }
        ctx->has_info = true;
    }
    // objects/arrays/null: skip
}

static void metrics_prom_emit_info(metrics_walk_ctx_t *ctx)
{
    if (!ctx->has_info || ctx->err != BB_OK) return;
    char name[128];
    char sanitized_sub[64];
    metrics_sanitize_name(ctx->subtopic, sanitized_sub, sizeof(sanitized_sub));
    snprintf(name, sizeof(name), "%s_%s_info", ctx->prefix, sanitized_sub);
    char sanitized_name[128];
    metrics_sanitize_name(name, sanitized_name, sizeof(sanitized_name));

    char host_escaped[128];
    metrics_escape_label_val(ctx->host ? ctx->host : "", host_escaped, sizeof(host_escaped));

    char line[1024];
    snprintf(line, sizeof(line), "# TYPE %s gauge\n", sanitized_name);
    if (metrics_chunk(ctx->req, line) != BB_OK) { ctx->err = BB_ERR_INVALID_STATE; return; }
    if (!ctx->schema_only) {
        snprintf(line, sizeof(line), "%s{host=\"%s\"%s} 1\n",
                 sanitized_name, host_escaped, ctx->info_labels);
        if (metrics_chunk(ctx->req, line) != BB_OK) { ctx->err = BB_ERR_INVALID_STATE; return; }
    }
}

// ---------------------------------------------------------------------------
// JSON schema walk callback (builds metrics[] array items into jstream)
// ---------------------------------------------------------------------------

typedef struct {
    bb_http_json_obj_stream_t *jstream;
    const char                *prefix;
    const char                *subtopic;
    bb_err_t                   err;
} metrics_schema_json_ctx_t;

static void metrics_schema_json_walk_cb(const char *key, bb_json_t child, void *ctx_)
{
    metrics_schema_json_ctx_t *ctx = (metrics_schema_json_ctx_t *)ctx_;
    if (ctx->err != BB_OK) return;
    if (!key) return;
    if (strcmp(key, "ts") == 0 || strcmp(key, "transport") == 0 ||
        strcmp(key, "tls") == 0) return;

    char name[128];
    metrics_make_name(ctx->prefix, ctx->subtopic, key, name, sizeof(name));

    bb_http_resp_json_obj_set_obj_begin(ctx->jstream, NULL);
    bb_http_resp_json_obj_set_str(ctx->jstream, "name", name);
    bb_http_resp_json_obj_set_str(ctx->jstream, "type", "gauge");
    bb_http_resp_json_obj_set_str(ctx->jstream, "source", ctx->subtopic);
    ctx->err = bb_http_resp_json_obj_set_obj_end(ctx->jstream);
    (void)child;
}

// ---------------------------------------------------------------------------
// JSON value walk callback (builds source sub-object into jstream)
// ---------------------------------------------------------------------------

static void metrics_json_val_walk_cb(const char *key, bb_json_t child, void *ctx_)
{
    bb_http_json_obj_stream_t *jstream = (bb_http_json_obj_stream_t *)ctx_;
    if (!key) return;
    if (strcmp(key, "ts") == 0 || strcmp(key, "transport") == 0 ||
        strcmp(key, "tls") == 0) return;

    if (bb_json_item_is_number(child)) {
        bb_http_resp_json_obj_set_num(jstream, key, bb_json_item_get_double(child));
    } else if (bb_json_item_is_true(child)) {
        bb_http_resp_json_obj_set_bool(jstream, key, true);
    } else if (bb_json_item_is_null(child)) {
        bb_http_resp_json_obj_set_null(jstream, key);
    } else if (bb_json_item_is_string(child)) {
        bb_http_resp_json_obj_set_str(jstream, key, bb_json_item_get_string(child));
    } else {
        if (!bb_json_item_is_array(child) && !bb_json_item_is_object(child)) {
            bb_http_resp_json_obj_set_bool(jstream, key, false);
        }
    }
}

// ---------------------------------------------------------------------------
// Publisher health gauges (shared between prom and json)
// ---------------------------------------------------------------------------

static void metrics_emit_pub_health_prom(bb_http_request_t *req, const char *prefix,
                                          const char *host, bool schema_only, bb_err_t *err)
{
    if (*err != BB_OK) return;
    char host_escaped[128];
    metrics_escape_label_val(host ? host : "", host_escaped, sizeof(host_escaped));

    bb_pub_buffer_stats_t bstats = {0};
    bb_pub_buffer_stats(&bstats);

    bb_pub_status_t st = {0};
    bb_pub_get_status(&st);
    uint32_t now_ms = bb_clock_now_ms();
    uint32_t age_ms = st.last_publish_ms ? (now_ms - st.last_publish_ms) : 0;

    struct {
        const char *suffix;
        double      value;
    } all_gauges[] = {
        { "pub_source_count",     (double)bb_pub_source_count()    },
        { "pub_buffer_count",     (double)bstats.count             },
        { "pub_buffer_dropped",   (double)bstats.dropped           },
        { "pub_ring_undersized",  bb_pub_ring_undersized() ? 1.0 : 0.0 },
        { "pub_last_publish_age_ms", (double)age_ms                },
    };

    for (size_t i = 0; i < sizeof(all_gauges)/sizeof(all_gauges[0]); i++) {
        char name[128];
        char sani[128];
        snprintf(name, sizeof(name), "%s_%s", prefix, all_gauges[i].suffix);
        metrics_sanitize_name(name, sani, sizeof(sani));
        char line[256];
        snprintf(line, sizeof(line), "# TYPE %s gauge\n", sani);
        if (metrics_chunk(req, line) != BB_OK) { *err = BB_ERR_INVALID_STATE; return; }
        if (!schema_only) {
            snprintf(line, sizeof(line), "%s{host=\"%s\"} %.6g\n",
                     sani, host_escaped, all_gauges[i].value);
            if (metrics_chunk(req, line) != BB_OK) { *err = BB_ERR_INVALID_STATE; return; }
        }
    }
}

static void metrics_emit_pub_health_json(bb_http_json_obj_stream_t *jstream,
                                          const char *prefix, bool schema_only)
{
    if (schema_only) return;
    (void)prefix;

    bb_pub_buffer_stats_t bstats = {0};
    bb_pub_buffer_stats(&bstats);

    bb_pub_status_t st = {0};
    bb_pub_get_status(&st);
    uint32_t now_ms = bb_clock_now_ms();
    uint32_t age_ms = st.last_publish_ms ? (now_ms - st.last_publish_ms) : 0;

    bb_http_resp_json_obj_set_obj_begin(jstream, "publisher");
    bb_http_resp_json_obj_set_int(jstream, "source_count",     (int64_t)bb_pub_source_count());
    bb_http_resp_json_obj_set_int(jstream, "buffer_count",     (int64_t)bstats.count);
    bb_http_resp_json_obj_set_int(jstream, "buffer_dropped",   (int64_t)bstats.dropped);
    bb_http_resp_json_obj_set_bool(jstream, "ring_undersized", bb_pub_ring_undersized());
    bb_http_resp_json_obj_set_int(jstream, "last_publish_age_ms", (int64_t)age_ms);
    bb_http_resp_json_obj_end(jstream);
}

// ---------------------------------------------------------------------------
// GET /api/telemetry/metrics handler
// ---------------------------------------------------------------------------

static bb_err_t metrics_handler(bb_http_request_t *req)
{
    char fmt_buf[16]    = "prom";
    char schema_buf[8]  = "";
    bool want_json   = false;
    bool schema_only = false;

    bb_http_req_query_key_value(req, "format", fmt_buf, sizeof(fmt_buf));
    if (strncmp(fmt_buf, "json", 4) == 0) want_json = true;

    if (bb_http_req_query_key_value(req, "schema", schema_buf, sizeof(schema_buf)) == BB_OK) {
        schema_only = true;
    }

    const char *prefix = bb_pub_metrics_prefix();
    const char *host   = bb_nv_config_hostname();

    // -----------------------------------------------------------------------
    // SCHEMA / JSON
    // -----------------------------------------------------------------------
    if (schema_only && want_json) {
        bb_http_json_obj_stream_t jstream;
        bb_err_t err = bb_http_resp_json_obj_begin(req, &jstream);
        if (err != BB_OK) return err;

        bb_http_resp_json_obj_set_str(&jstream, "prefix", prefix);

        bb_http_resp_json_obj_set_arr_begin(&jstream, "metrics");
        int n = bb_pub_source_count();
        for (int i = 0; i < n; i++) {
            const char *subtopic = NULL;
            bb_pub_sample_fn fn  = NULL;
            void *ctx            = NULL;
            if (bb_pub_source_info(i, &subtopic, &fn, &ctx, NULL, NULL) != BB_OK) continue;

            bb_json_t obj = bb_json_obj_new();
            if (!obj) continue;
            bool ok = fn(obj, ctx);
            if (ok) {
                metrics_schema_json_ctx_t sctx = {
                    .jstream  = &jstream,
                    .prefix   = prefix,
                    .subtopic = subtopic,
                    .err      = BB_OK,
                };
                bb_json_walk_children(obj, metrics_schema_json_walk_cb, &sctx);
            }
            bb_json_free(obj);
        }
        const char *pub_metrics[] = {
            "pub_source_count", "pub_buffer_count", "pub_buffer_dropped",
            "pub_ring_undersized", "pub_last_publish_age_ms"
        };
        for (size_t pi = 0; pi < sizeof(pub_metrics)/sizeof(pub_metrics[0]); pi++) {
            char name[128];
            snprintf(name, sizeof(name), "%s_%s", prefix, pub_metrics[pi]);
            char sani[128];
            metrics_sanitize_name(name, sani, sizeof(sani));
            bb_http_resp_json_obj_set_obj_begin(&jstream, NULL);
            bb_http_resp_json_obj_set_str(&jstream, "name", sani);
            bb_http_resp_json_obj_set_str(&jstream, "type", "gauge");
            bb_http_resp_json_obj_set_str(&jstream, "source", "publisher");
            bb_http_resp_json_obj_set_obj_end(&jstream);
        }
        bb_http_resp_json_obj_set_arr_end(&jstream);

        bb_http_resp_json_obj_set_arr_begin(&jstream, "publisher");
        for (size_t pi = 0; pi < sizeof(pub_metrics)/sizeof(pub_metrics[0]); pi++) {
            bb_http_resp_json_obj_set_obj_begin(&jstream, NULL);
            bb_http_resp_json_obj_set_str(&jstream, "name", pub_metrics[pi]);
            bb_http_resp_json_obj_set_str(&jstream, "type", "gauge");
            bb_http_resp_json_obj_set_obj_end(&jstream);
        }
        bb_http_resp_json_obj_set_arr_end(&jstream);

        return bb_http_resp_json_obj_end(&jstream);
    }

    // -----------------------------------------------------------------------
    // VALUES / JSON
    // -----------------------------------------------------------------------
    if (!schema_only && want_json) {
        bb_http_json_obj_stream_t jstream;
        bb_err_t err = bb_http_resp_json_obj_begin(req, &jstream);
        if (err != BB_OK) return err;

        bb_http_resp_json_obj_set_str(&jstream, "host", host);
        bb_http_resp_json_obj_set_int(&jstream, "ts_ms", (int64_t)bb_clock_now_ms());

        bb_http_resp_json_obj_set_obj_begin(&jstream, "sources");
        int n = bb_pub_source_count();
        for (int i = 0; i < n; i++) {
            const char *subtopic = NULL;
            bb_pub_sample_fn fn  = NULL;
            void *ctx            = NULL;
            if (bb_pub_source_info(i, &subtopic, &fn, &ctx, NULL, NULL) != BB_OK) continue;

            bb_json_t obj = bb_json_obj_new();
            if (!obj) continue;
            bool ok = fn(obj, ctx);
            if (ok) {
                bb_http_resp_json_obj_set_obj_begin(&jstream, subtopic);
                bb_json_walk_children(obj, metrics_json_val_walk_cb, &jstream);
                bb_http_resp_json_obj_set_obj_end(&jstream);
            }
            bb_json_free(obj);
        }
        bb_http_resp_json_obj_set_obj_end(&jstream);  // end sources

        metrics_emit_pub_health_json(&jstream, prefix, false);

        return bb_http_resp_json_obj_end(&jstream);
    }

    // -----------------------------------------------------------------------
    // SCHEMA / PROM  or  VALUES / PROM  (both text/plain chunked)
    // -----------------------------------------------------------------------
    bb_err_t err = bb_http_resp_set_type(req, "text/plain; version=0.0.4");
    if (err != BB_OK) return err;

    int n = bb_pub_source_count();
    for (int i = 0; i < n; i++) {
        const char *subtopic = NULL;
        bb_pub_sample_fn fn  = NULL;
        void *ctx            = NULL;
        if (bb_pub_source_info(i, &subtopic, &fn, &ctx, NULL, NULL) != BB_OK) continue;

        bb_json_t obj = bb_json_obj_new();
        if (!obj) continue;
        bool ok = fn(obj, ctx);
        if (ok) {
            metrics_walk_ctx_t wctx = {
                .req         = req,
                .prefix      = prefix,
                .subtopic    = subtopic,
                .host        = host,
                .schema_only = schema_only,
                .err         = BB_OK,
                .info_labels = { 0 },
                .has_info    = false,
            };
            bb_json_walk_children(obj, metrics_prom_walk_cb, &wctx);
            metrics_prom_emit_info(&wctx);
            if (wctx.err != BB_OK) {
                bb_json_free(obj);
                return wctx.err;
            }
        }
        bb_json_free(obj);
    }

    metrics_emit_pub_health_prom(req, prefix, host, schema_only, &err);
    if (err != BB_OK) return err;

    return bb_http_resp_send_chunk(req, NULL, 0);
}

static const bb_route_response_t s_metrics_responses[] = {
    { 200, "text/plain",        NULL,
      "Prometheus exposition format (format=prom, default)" },
    { 200, "application/json", NULL,
      "JSON telemetry snapshot or schema descriptor (format=json)" },
    { 0 },
};

static const bb_route_param_t s_metrics_params[] = {
    { "format", "query",
      "Response format: 'prom' (default) for Prometheus exposition text, "
      "'json' for a JSON snapshot",
      false, "string" },
    { "schema", "query",
      "When present (bare key, no value), return the metric contract "
      "(TYPE/HELP lines or JSON descriptor) instead of live values",
      false, "string" },
};

static const bb_route_t s_metrics_route = {
    .method           = BB_HTTP_GET,
    .path             = "/api/telemetry/metrics",
    .tag              = "metrics",
    .summary          = "Prometheus-style telemetry snapshot",
    .responses        = s_metrics_responses,
    .handler          = metrics_handler,
    .parameters       = s_metrics_params,
    .parameters_count = 2,
};

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------

bb_err_t bb_telemetry_init(bb_http_handle_t server)
{
    // Register GET /api/telemetry/metrics first so it is available even on the host
    // capture harness (server == NULL): the route registry accepts a NULL
    // server and the metrics handler has no per-server state.
    bb_err_t mrc = bb_http_register_described_route(server, &s_metrics_route);
    if (mrc != BB_OK) {
        bb_log_e(TAG, "failed to register /api/telemetry/metrics: %d", mrc);
        return mrc;
    }
    bb_log_i(TAG, "registered GET /api/telemetry/metrics (prefix=%s)", bb_pub_metrics_prefix());

    if (!server) return BB_ERR_INVALID_ARG;

    // Build real composed GET schema from registered section schema_props.
    // Sections must be registered (PRE_HTTP tier) before this init (order 5).
    char *schema = bb_telemetry_assemble_get_schema();
    if (!schema) {
        bb_log_w(TAG, "schema assembly: malloc failed; GET schema will be NULL");
    }
    s_telemetry_get_responses[0].schema = schema;

    // Freeze: reject late registrations (all sections must be PRE_HTTP).
    bb_telemetry_freeze();

    bb_err_t rc = bb_http_register_described_route(server, &s_telemetry_get_route);
    if (rc != BB_OK) return rc;
    rc = bb_http_register_described_route(server, &s_telemetry_patch_route);
    if (rc != BB_OK) return rc;

    bb_log_i(TAG, "telemetry routes registered");
    return BB_OK;
}

#if CONFIG_BB_TELEMETRY_AUTOREGISTER
BB_REGISTRY_REGISTER_N(bb_telemetry, bb_telemetry_init, 5);
#endif
