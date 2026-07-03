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
#include "bb_tls.h"
#include "bb_nv_namespaces.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

#define BB_SINK_HTTP_BASE_MAX     128
#define BB_SINK_HTTP_PATH_MAX     128
#define BB_SINK_HTTP_PATH_DEFAULT "/topics/{topic}?qos={qos}"

// Per-header limits and list cap.
#define BB_SINK_HTTP_HEADERS_MAX       8
#define BB_SINK_HTTP_HEADER_NAME_MAX   64
#define BB_SINK_HTTP_HEADER_VALUE_MAX  256
#define BB_SINK_HTTP_CLIENT_ID_MAX     64

// A single configurable request header.
typedef struct {
    char name[BB_SINK_HTTP_HEADER_NAME_MAX];
    char value[BB_SINK_HTTP_HEADER_VALUE_MAX];
    bool secret;  // when true: value omitted from GET, preserved on PATCH if blank
} bb_sink_http_header_t;

typedef struct {
    char base[BB_SINK_HTTP_BASE_MAX];       // base URL, e.g. https://host:8443
    char path_tmpl[BB_SINK_HTTP_PATH_MAX];  // path template; default if empty
    int  qos;                              // QoS value substituted into {qos}
    bool enabled;                          // when false, publish is a no-op
    // client_id: sent as X-Client-Id header; defaults to bb_nv_config_hostname() when empty.
    char client_id[BB_SINK_HTTP_CLIENT_ID_MAX];
    bb_sink_http_header_t headers[BB_SINK_HTTP_HEADERS_MAX];
    int  num_headers;
} bb_sink_http_cfg_t;

// ---------------------------------------------------------------------------
// Health snapshot for /api/health net.http sub-block.
// ---------------------------------------------------------------------------

typedef struct {
    bool         connected;        // true when a session is currently open
    int          consec_failures;  // consecutive transport failures
    bb_tls_fail_t tls_fail;        // last classified TLS failure (NONE if none)
    int          last_status;      // last HTTP status code (0 if no successful response)
} bb_sink_http_health_t;

/**
 * Return a snapshot of the current HTTP sink health state.
 * Thread-safe (reads under a mutex). Always returns BB_OK.
 */
bb_err_t bb_sink_http_get_health(bb_sink_http_health_t *out);

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

// ---------------------------------------------------------------------------
// Delimited NVS format helpers (pure, host-testable)
//
// Format: one header per line, '\n'-separated.
//   <*>name: value
// A leading '*' marks the header SECRET (stripped from name).
// Name and value are split on the FIRST ": " (colon-space).
// Blank or malformed lines are skipped.
//
// Validation (security):
//   - Header names must be RFC 7230 tokens: no ':', whitespace, or control chars.
//   - Header values must not contain '\r' or '\n' (HTTP injection guard).
//   - Entries failing validation are silently skipped on parse, rejected on set.
// ---------------------------------------------------------------------------

// Parse a delimited NVS string into an array of headers.
// Returns the number of valid entries written to `out` (≤ BB_SINK_HTTP_HEADERS_MAX).
// `buf` is a '\0'-terminated delimited string (may be NULL → returns 0).
int bb_sink_http_parse_headers(const char *buf,
                                bb_sink_http_header_t *out, int out_max);

// Serialize headers to a delimited NVS string.
// `dst` receives the result; at most dst_cap-1 bytes written plus NUL terminator.
// Returns the number of bytes written (excluding NUL).
size_t bb_sink_http_serialize_headers(const bb_sink_http_header_t *headers,
                                       int num_headers,
                                       char *dst, size_t dst_cap);

// ---------------------------------------------------------------------------
// PATCH merge helper (pure, host-testable)
//
// Merges an incoming PATCH headers array against the existing stored list.
// Rules:
//   - entry secret==true + absent/empty value → preserve existing stored value by name.
//   - entry secret==true + non-empty value → update.
//   - entry secret==false → use provided value (absent ⇒ empty).
//   - any name NOT in the submitted array → removed.
// Empty-name entries are rejected; caps are enforced.
// Writes result to `out`; returns number of valid entries.
// ---------------------------------------------------------------------------
typedef struct {
    char  name[BB_SINK_HTTP_HEADER_NAME_MAX];
    char  value[BB_SINK_HTTP_HEADER_VALUE_MAX];  // may be empty (absent in PATCH)
    bool  secret;
    bool  value_present;  // true if the PATCH body included a "value" key
} bb_sink_http_patch_entry_t;

int bb_sink_http_merge_headers(const bb_sink_http_patch_entry_t *patch, int patch_count,
                                const bb_sink_http_header_t *existing, int existing_count,
                                bb_sink_http_header_t *out, int out_max);

// ---------------------------------------------------------------------------
// Validation helpers (pure, used by parse and merge)
// ---------------------------------------------------------------------------

// Returns true if the name is a valid RFC 7230 token (no ':', whitespace, or controls).
bool bb_sink_http_header_name_valid(const char *name);

// Returns true if the value contains no '\r' or '\n'.
bool bb_sink_http_header_value_valid(const char *value);

// ---------------------------------------------------------------------------
// Testing hooks (BB_SINK_HTTP_TESTING only)
// ---------------------------------------------------------------------------
#ifdef BB_SINK_HTTP_TESTING
// Inject a custom malloc for heap-failure tests.
void bb_sink_http_set_malloc(void *(*fn)(size_t));
void bb_sink_http_reset_malloc(void);
// Inject health state for testing (bypasses the real publish path).
void bb_sink_http_test_set_health(bool connected, int consec_failures, bb_tls_fail_t tls_fail, int last_status);
void bb_sink_http_test_reset_health(void);
// Reset the lazily-registered bb_transport_health handle so tests that also
// call bb_transport_health_reset_for_test() don't leave the sink holding a
// stale (now-unused) slot index.
void bb_sink_http_reset_transport_health_for_test(void);
#endif /* BB_SINK_HTTP_TESTING */

#ifdef __cplusplus
}
#endif
