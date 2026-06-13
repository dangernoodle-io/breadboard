// bb_sink_http — HTTP-publish sink adapter for bb_pub.
// Compiled on both host (tests) and ESP-IDF (via CMakeLists.txt SRCS path).
#include "bb_sink_http.h"
#include "bb_http_client.h"
#include "bb_tls_creds.h"
#include "bb_nv.h"
#include "bb_log.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "bb_sink_http";

#ifndef CONFIG_BB_SINK_HTTP_RESP_BUF_BYTES
#define CONFIG_BB_SINK_HTTP_RESP_BUF_BYTES 256
#endif

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------

static bb_sink_http_cfg_t         s_cfg;
static bool                      s_initialized = false;
static bb_http_client_session_t  s_session      = NULL;
static bb_tls_creds_t            s_creds;        // kept alive for session lifetime

// ---------------------------------------------------------------------------
// NVS helpers
// ---------------------------------------------------------------------------

static void load_from_nvs(bb_sink_http_cfg_t *out)
{
    memset(out, 0, sizeof(*out));
    bb_nv_get_str(BB_SINK_HTTP_NVS_NS, "base",
                  out->base, sizeof(out->base), "");
    bb_nv_get_str(BB_SINK_HTTP_NVS_NS, "path_tmpl",
                  out->path_tmpl, sizeof(out->path_tmpl), "");

    char qos_str[8] = "1";
    bb_nv_get_str(BB_SINK_HTTP_NVS_NS, "qos", qos_str, sizeof(qos_str), "1");
    out->qos = (int)qos_str[0] - '0';
    if (out->qos < 0 || out->qos > 2) out->qos = 1;

    char enabled_str[4] = "0";
    bb_nv_get_str(BB_SINK_HTTP_NVS_NS, "enabled",
                  enabled_str, sizeof(enabled_str), "0");
    out->enabled = (enabled_str[0] == '1');
}

static void save_to_nvs(const bb_sink_http_cfg_t *cfg)
{
    bb_nv_set_str(BB_SINK_HTTP_NVS_NS, "base",      cfg->base);
    bb_nv_set_str(BB_SINK_HTTP_NVS_NS, "path_tmpl", cfg->path_tmpl);

    char qos_str[4] = {0};
    qos_str[0] = (char)('0' + cfg->qos);
    bb_nv_set_str(BB_SINK_HTTP_NVS_NS, "qos",     qos_str);
    bb_nv_set_str(BB_SINK_HTTP_NVS_NS, "enabled", cfg->enabled ? "1" : "0");
}

// ---------------------------------------------------------------------------
// URL-encode helper (pure, host-testable)
// ---------------------------------------------------------------------------

size_t bb_sink_http_url_encode(const char *src, char *dst, size_t dst_cap)
{
    if (!src || !dst || dst_cap == 0) return 0;

    static const char hex[] = "0123456789ABCDEF";
    size_t out = 0;

    for (; *src && out + 1 < dst_cap; src++) {
        unsigned char c = (unsigned char)*src;
        // Unreserved characters per RFC 3986 §2.3 (pass through unchanged).
        if ((c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            dst[out++] = (char)c;
        } else {
            // Percent-encode (need 3 bytes: %XY).
            if (out + 3 >= dst_cap) break;
            dst[out++] = '%';
            dst[out++] = hex[(c >> 4) & 0xF];
            dst[out++] = hex[c & 0xF];
        }
    }
    dst[out] = '\0';
    return out;
}

// ---------------------------------------------------------------------------
// URL builder: base + path_tmpl with {topic} and {qos} substitution
// ---------------------------------------------------------------------------

static size_t build_url(const bb_sink_http_cfg_t *cfg,
                        const char *topic,
                        char *dst, size_t dst_cap)
{
    const char *tmpl = cfg->path_tmpl[0] ? cfg->path_tmpl : BB_SINK_HTTP_PATH_DEFAULT;

    // URL-encode the topic.
    char enc_topic[256];
    bb_sink_http_url_encode(topic, enc_topic, sizeof(enc_topic));

    // Build qos string.
    char qos_str[4];
    snprintf(qos_str, sizeof(qos_str), "%d", cfg->qos);

    // Start with base.
    size_t pos = 0;
    size_t base_len = strlen(cfg->base);
    if (base_len >= dst_cap) return 0;
    memcpy(dst, cfg->base, base_len);
    pos = base_len;

    // Walk path_tmpl, substituting placeholders.
    for (const char *p = tmpl; *p && pos + 1 < dst_cap; ) {
        if (p[0] == '{') {
            if (strncmp(p, "{topic}", 7) == 0) {
                size_t n = strlen(enc_topic);
                if (pos + n + 1 >= dst_cap) break;
                memcpy(dst + pos, enc_topic, n);
                pos += n;
                p += 7;
            } else if (strncmp(p, "{qos}", 5) == 0) {
                size_t n = strlen(qos_str);
                if (pos + n + 1 >= dst_cap) break;
                memcpy(dst + pos, qos_str, n);
                pos += n;
                p += 5;
            } else {
                dst[pos++] = *p++;
            }
        } else {
            dst[pos++] = *p++;
        }
    }
    dst[pos] = '\0';
    return pos;
}

// ---------------------------------------------------------------------------
// Session lifecycle helpers
// ---------------------------------------------------------------------------

static void session_close(void)
{
    if (s_session) {
        bb_http_client_session_close(s_session);
        s_session = NULL;
    }
    bb_tls_creds_free(&s_creds);
}

// Ensure the session is open.  Opens lazily on first call; resolves TLS
// credentials once and keeps them alive for the session lifetime.
static bb_err_t session_ensure(void)
{
    if (s_session) return BB_OK;

    memset(&s_creds, 0, sizeof(s_creds));
    bb_err_t rc = bb_tls_creds_resolve(BB_SINK_HTTP_NVS_NS, NULL, &s_creds);
    if (rc != BB_OK) {
        bb_log_e(TAG, "tls_creds_resolve failed: %d", rc);
        return rc;
    }

    bb_http_client_cfg_t http_cfg;
    memset(&http_cfg, 0, sizeof(http_cfg));
    http_cfg.ca_cert_pem     = s_creds.ca;
    http_cfg.client_cert_pem = s_creds.cert;
    http_cfg.client_key_pem  = s_creds.key;

    rc = bb_http_client_session_open(&http_cfg, s_cfg.base, &s_session);
    if (rc != BB_OK) {
        bb_log_e(TAG, "session_open failed: %d", rc);
        bb_tls_creds_free(&s_creds);
        return rc;
    }

    bb_log_i(TAG, "session opened to %s", s_cfg.base);
    return BB_OK;
}

// ---------------------------------------------------------------------------
// Sink publish function
// ---------------------------------------------------------------------------

static bb_err_t http_pub_publish(void *ctx,
                                  const char *topic,
                                  const char *payload, int len)
{
    (void)ctx;

    if (!s_initialized) {
        bb_log_e(TAG, "not initialized");
        return BB_ERR_INVALID_STATE;
    }

    if (!s_cfg.enabled) {
        bb_log_d(TAG, "disabled — skipping publish");
        return BB_OK;
    }

    // Build URL for this topic.
    char url[BB_SINK_HTTP_BASE_MAX + BB_SINK_HTTP_PATH_MAX + 512];
    if (!build_url(&s_cfg, topic, url, sizeof(url))) {
        bb_log_e(TAG, "url build failed (base or path too long)");
        return BB_ERR_INVALID_ARG;
    }

    // Ensure one reusable session is open (lazy open, keep-alive).
    bb_err_t rc = session_ensure();
    if (rc != BB_OK) return rc;

    bb_http_client_result_t out;
    rc = bb_http_client_session_post(s_session, url, payload, (size_t)len,
                                      "application/json", &out);
    if (rc != BB_OK) {
        bb_log_e(TAG, "session POST transport error: %d", rc);
        // Keep the handle; perform() reconnects on next call.
        return rc;
    }

    bb_log_d(TAG, "published to %s -> %d", url, out.status_code);
    return BB_OK;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bb_err_t bb_sink_http_init(const bb_sink_http_cfg_t *over)
{
    // Close any existing session — config may have changed.
    session_close();

    load_from_nvs(&s_cfg);

    if (over) {
        if (over->base[0])      {
            strncpy(s_cfg.base, over->base, sizeof(s_cfg.base) - 1);
            s_cfg.base[sizeof(s_cfg.base) - 1] = '\0';
        }
        if (over->path_tmpl[0]) {
            strncpy(s_cfg.path_tmpl, over->path_tmpl, sizeof(s_cfg.path_tmpl) - 1);
            s_cfg.path_tmpl[sizeof(s_cfg.path_tmpl) - 1] = '\0';
        }
        if (over->qos != 0)     s_cfg.qos     = over->qos;
        if (over->enabled)      s_cfg.enabled  = over->enabled;
    }

    s_initialized = true;
    bb_log_i(TAG, "init: base=%s enabled=%d qos=%d",
             s_cfg.base, s_cfg.enabled, s_cfg.qos);
    return BB_OK;
}

void bb_sink_http_get_cfg(bb_sink_http_cfg_t *out)
{
    if (!out) return;
    *out = s_cfg;
}

bb_err_t bb_sink_http_set_cfg(const bb_sink_http_cfg_t *cfg)
{
    if (!cfg) return BB_ERR_INVALID_ARG;
    // Config changed — close the session so the next publish reopens with new creds/base.
    session_close();
    s_cfg = *cfg;
    save_to_nvs(cfg);
    return BB_OK;
}

bb_err_t bb_sink_http(bb_pub_sink_t *out)
{
    if (!out) return BB_ERR_INVALID_ARG;
    out->publish = http_pub_publish;
    out->ctx     = NULL;
    return BB_OK;
}
