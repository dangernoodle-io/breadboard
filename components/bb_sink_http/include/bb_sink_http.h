// bb_sink_http — HTTP-publish sink adapter for bb_pub.
//
// Bridges bb_http_client_post into a bb_pub_sink_t so the transport-agnostic
// bb_pub core can deliver telemetry over HTTP. Useful for AWS IoT Core HTTPS
// publish, EMQX REST API, or any HTTP endpoint that accepts JSON bodies.
//
// Config is loaded from NVS namespace "bb_sink_http" at init and refreshed on
// each PATCH /api/httppub. TLS credentials are resolved via bb_tls_creds from
// the same namespace.
//
// URL shape:
//   <base><path_tmpl>  where {topic} -> URL-encoded topic, {qos} -> qos int
//   Default path_tmpl: /topics/{topic}?qos={qos}   (AWS IoT HTTPS publish shape)
//
// Usage:
//   bb_sink_http_cfg_t cfg = { .base = "https://xxxx-ats.iot.us-east-1.amazonaws.com:8443" };
//   bb_sink_http_init(&cfg);  // or pass NULL to load from NVS only
//   bb_pub_sink_t sink;
//   bb_sink_http(&sink);
//   bb_pub_set_sink(&sink);
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "bb_core.h"
#include "bb_pub.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

#define BB_SINK_HTTP_NVS_NS       "bb_sink_http"
#define BB_SINK_HTTP_BASE_MAX     128
#define BB_SINK_HTTP_PATH_MAX     128
#define BB_SINK_HTTP_PATH_DEFAULT "/topics/{topic}?qos={qos}"

typedef struct {
    char base[BB_SINK_HTTP_BASE_MAX];       // base URL, e.g. https://host:8443
    char path_tmpl[BB_SINK_HTTP_PATH_MAX];  // path template; default if empty
    int  qos;                              // QoS value substituted into {qos}
    bool enabled;                          // when false, publish is a no-op
} bb_sink_http_cfg_t;

// ---------------------------------------------------------------------------
// Init / cfg
// ---------------------------------------------------------------------------

/**
 * Initialise bb_sink_http from NVS, then apply any non-empty fields from `over`
 * (programmatic override — pass NULL to use NVS only).
 * Must be called before bb_sink_http().
 */
bb_err_t bb_sink_http_init(const bb_sink_http_cfg_t *over);

/** Return a copy of the currently active configuration. */
void bb_sink_http_get_cfg(bb_sink_http_cfg_t *out);

/** Replace the active configuration and persist to NVS. */
bb_err_t bb_sink_http_set_cfg(const bb_sink_http_cfg_t *cfg);

// ---------------------------------------------------------------------------
// Sink factory
// ---------------------------------------------------------------------------

/**
 * Fill `out` with a bb_pub_sink_t that publishes via bb_http_client_post.
 * Callers must call bb_sink_http_init() first.
 *
 * @return BB_OK on success; BB_ERR_INVALID_ARG if out is NULL.
 */
bb_err_t bb_sink_http(bb_pub_sink_t *out);

// ---------------------------------------------------------------------------
// URL-encode helper (pure, host-testable)
// Encodes reserved URL characters; '/' -> %2F.
// Returns number of bytes written (excluding NUL).  Writes a NUL terminator.
// If dst is too small the output is truncated and NUL-terminated.
// ---------------------------------------------------------------------------
size_t bb_sink_http_url_encode(const char *src, char *dst, size_t dst_cap);

#ifdef __cplusplus
}
#endif
