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

// NVS key for the delimited headers string.
#define HEADERS_NVS_KEY "headers"
// Maximum NVS buffer for all serialized headers.
#define HEADERS_BUF_MAX 2048

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------

static bb_sink_http_cfg_t         s_cfg;
static bool                      s_initialized = false;
static bb_http_client_session_t  s_session      = NULL;
static bb_tls_creds_t            s_creds;        // kept alive for session lifetime
static int                       s_consec_failures = 0;  // consecutive transport failures
#define BB_SINK_HTTP_MAX_CONSEC_FAILURES 3

// ---------------------------------------------------------------------------
// Validation helpers (pure)
// ---------------------------------------------------------------------------

bool bb_sink_http_header_name_valid(const char *name)
{
    if (!name || name[0] == '\0') return false;
    for (const char *p = name; *p; p++) {
        unsigned char c = (unsigned char)*p;
        // RFC 7230 token: visible ASCII except delimiters.
        // Reject ':', whitespace (space/tab), and control chars (<= 0x1F or 0x7F).
        if (c <= 0x1F || c == 0x7F) return false;
        if (c == ':' || c == ' ' || c == '\t') return false;
    }
    return true;
}

bool bb_sink_http_header_value_valid(const char *value)
{
    if (!value) return false;
    for (const char *p = value; *p; p++) {
        if (*p == '\r' || *p == '\n') return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Parse delimited NVS string → header array (pure)
// ---------------------------------------------------------------------------

int bb_sink_http_parse_headers(const char *buf,
                                bb_sink_http_header_t *out, int out_max)
{
    if (!buf || !out || out_max <= 0) return 0;

    int count = 0;
    // Work on a copy since we mutate with NUL terminators.
    char tmp[HEADERS_BUF_MAX];
    strncpy(tmp, buf, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    char *line = tmp;
    while (*line && count < out_max) {
        // Find end of this line.
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';

        // Skip blank lines.
        if (line[0] == '\0') {
            line = nl ? nl + 1 : line + strlen(line);
            continue;
        }

        // Check for secret prefix.
        bool secret = false;
        char *p = line;
        if (*p == '*') {
            secret = true;
            p++;
        }

        // Split on first ": " (colon-space).
        char *sep = strstr(p, ": ");
        if (!sep) {
            // Malformed — skip.
            line = nl ? nl + 1 : p + strlen(p);
            continue;
        }
        *sep = '\0';
        char *name  = p;
        char *value = sep + 2;  // skip ": "

        // Validate.
        if (!bb_sink_http_header_name_valid(name) ||
            !bb_sink_http_header_value_valid(value)) {
            line = nl ? nl + 1 : value + strlen(value);
            continue;
        }

        // Enforce max lengths.
        if (strlen(name) >= BB_SINK_HTTP_HEADER_NAME_MAX ||
            strlen(value) >= BB_SINK_HTTP_HEADER_VALUE_MAX) {
            line = nl ? nl + 1 : value + strlen(value);
            continue;
        }

        bb_sink_http_header_t *h = &out[count++];
        strncpy(h->name,  name,  sizeof(h->name)  - 1);
        strncpy(h->value, value, sizeof(h->value) - 1);
        h->name[sizeof(h->name)   - 1] = '\0';
        h->value[sizeof(h->value) - 1] = '\0';
        h->secret = secret;

        line = nl ? nl + 1 : line + strlen(line) + 1;
    }
    return count;
}

// ---------------------------------------------------------------------------
// Serialize header array → delimited NVS string (pure)
// ---------------------------------------------------------------------------

size_t bb_sink_http_serialize_headers(const bb_sink_http_header_t *headers,
                                       int num_headers,
                                       char *dst, size_t dst_cap)
{
    if (!headers || !dst || dst_cap == 0) return 0;
    size_t pos = 0;

    for (int i = 0; i < num_headers && pos + 1 < dst_cap; i++) {
        const bb_sink_http_header_t *h = &headers[i];
        if (!bb_sink_http_header_name_valid(h->name) ||
            !bb_sink_http_header_value_valid(h->value)) {
            continue;
        }

        // prefix '*' for secret.
        if (h->secret) {
            if (pos + 1 >= dst_cap) break;
            dst[pos++] = '*';
        }

        // write name.
        size_t nlen = strlen(h->name);
        if (pos + nlen + 2 >= dst_cap) break;  // need ": " after
        memcpy(dst + pos, h->name, nlen);
        pos += nlen;

        // write ": ".
        dst[pos++] = ':';
        dst[pos++] = ' ';

        // write value.
        size_t vlen = strlen(h->value);
        if (pos + vlen + 1 >= dst_cap) {
            // Truncate value to fit.
            vlen = dst_cap - pos - 1;
        }
        if (vlen > 0) {
            memcpy(dst + pos, h->value, vlen);
            pos += vlen;
        }

        // separate entries with '\n' (no trailing newline needed, but harmless).
        if (i + 1 < num_headers && pos + 1 < dst_cap) {
            dst[pos++] = '\n';
        }
    }
    dst[pos] = '\0';
    return pos;
}

// ---------------------------------------------------------------------------
// PATCH merge helper (pure)
// ---------------------------------------------------------------------------

int bb_sink_http_merge_headers(const bb_sink_http_patch_entry_t *patch, int patch_count,
                                const bb_sink_http_header_t *existing, int existing_count,
                                bb_sink_http_header_t *out, int out_max)
{
    if (!out || out_max <= 0) return 0;
    if (!patch || patch_count <= 0) return 0;

    int count = 0;
    for (int i = 0; i < patch_count && count < out_max; i++) {
        const bb_sink_http_patch_entry_t *pe = &patch[i];
        if (pe->name[0] == '\0') continue;  // reject empty names
        if (!bb_sink_http_header_name_valid(pe->name)) continue;

        bb_sink_http_header_t *h = &out[count];
        strncpy(h->name, pe->name, sizeof(h->name) - 1);
        h->name[sizeof(h->name) - 1] = '\0';
        h->secret = pe->secret;

        if (pe->secret && (!pe->value_present || pe->value[0] == '\0')) {
            // Preserve existing value by name lookup.
            bool found = false;
            if (existing) {
                for (int j = 0; j < existing_count; j++) {
                    if (strcmp(existing[j].name, pe->name) == 0) {
                        strncpy(h->value, existing[j].value, sizeof(h->value) - 1);
                        h->value[sizeof(h->value) - 1] = '\0';
                        found = true;
                        break;
                    }
                }
            }
            if (!found) {
                h->value[0] = '\0';
            }
        } else {
            // Use provided value; validate it.
            if (!bb_sink_http_header_value_valid(pe->value)) continue;
            strncpy(h->value, pe->value, sizeof(h->value) - 1);
            h->value[sizeof(h->value) - 1] = '\0';
        }

        count++;
    }
    return count;
}

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

    bb_nv_get_str(BB_SINK_HTTP_NVS_NS, "client_id",
                  out->client_id, sizeof(out->client_id), "");

    char hbuf[HEADERS_BUF_MAX] = {0};
    bb_nv_get_str(BB_SINK_HTTP_NVS_NS, HEADERS_NVS_KEY, hbuf, sizeof(hbuf), "");
    out->num_headers = bb_sink_http_parse_headers(hbuf, out->headers, BB_SINK_HTTP_HEADERS_MAX);
}

static void save_to_nvs(const bb_sink_http_cfg_t *cfg)
{
    bb_nv_set_str(BB_SINK_HTTP_NVS_NS, "base",      cfg->base);
    bb_nv_set_str(BB_SINK_HTTP_NVS_NS, "path_tmpl", cfg->path_tmpl);
    bb_nv_set_str(BB_SINK_HTTP_NVS_NS, "client_id", cfg->client_id);

    char qos_str[4] = {0};
    qos_str[0] = (char)('0' + cfg->qos);
    bb_nv_set_str(BB_SINK_HTTP_NVS_NS, "qos",     qos_str);
    bb_nv_set_str(BB_SINK_HTTP_NVS_NS, "enabled", cfg->enabled ? "1" : "0");

    char hbuf[HEADERS_BUF_MAX] = {0};
    bb_sink_http_serialize_headers(cfg->headers, cfg->num_headers, hbuf, sizeof(hbuf));
    bb_nv_set_str(BB_SINK_HTTP_NVS_NS, HEADERS_NVS_KEY, hbuf);
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
// Apply all configured headers to an open session.
// ---------------------------------------------------------------------------

static void apply_headers_to_session(void)
{
    if (!s_session) return;

    // X-Client-Id: use client_id if set, else hostname.
    const char *cid = s_cfg.client_id[0] ? s_cfg.client_id : bb_nv_config_hostname();
    if (cid && cid[0]) {
        bb_http_client_session_set_header(s_session, "X-Client-Id", cid);
    }

    // User-configured headers.
    for (int i = 0; i < s_cfg.num_headers; i++) {
        const bb_sink_http_header_t *h = &s_cfg.headers[i];
        if (h->name[0]) {
            bb_http_client_session_set_header(s_session, h->name, h->value);
        }
    }
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
    // Gate: resolve TLS credentials only when BB_HTTP_TLS_ENABLE is on.
    // Default OFF = plaintext build; creds stay zeroed and session opens
    // without TLS (degradation mirrors Arduino-stub pattern used elsewhere).
#if CONFIG_BB_HTTP_TLS_ENABLE
    bb_err_t rc = bb_tls_creds_resolve(BB_SINK_HTTP_NVS_NS, NULL, &s_creds);
    if (rc != BB_OK) {
        bb_log_e(TAG, "tls_creds_resolve failed: %d", rc);
        return rc;
    }
#else
    bb_err_t rc = BB_OK;
#endif

    bb_http_client_cfg_t http_cfg;
    memset(&http_cfg, 0, sizeof(http_cfg));
#if CONFIG_BB_HTTP_TLS_ENABLE
    http_cfg.ca_cert_pem     = s_creds.ca;
#if CONFIG_BB_TLS_MUTUAL_ENABLE
    http_cfg.client_cert_pem = s_creds.cert;
    http_cfg.client_key_pem  = s_creds.key;
#endif /* CONFIG_BB_TLS_MUTUAL_ENABLE */
#endif /* CONFIG_BB_HTTP_TLS_ENABLE */
    // Keep-alive: respect Kconfig on ESP-IDF; default true on host/other targets.
#if defined(CONFIG_BB_HTTP_CLIENT_KEEPALIVE)
    http_cfg.keep_alive = CONFIG_BB_HTTP_CLIENT_KEEPALIVE;
#else
    http_cfg.keep_alive = true;
#endif

    rc = bb_http_client_session_open(&http_cfg, s_cfg.base, &s_session);
    if (rc != BB_OK) {
        bb_log_e(TAG, "session_open failed: %d", rc);
        bb_tls_creds_free(&s_creds);
        return rc;
    }

    // Apply headers to the freshly opened session.
    apply_headers_to_session();

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
        s_consec_failures++;
        if (s_consec_failures >= BB_SINK_HTTP_MAX_CONSEC_FAILURES) {
            bb_log_w(TAG, "%d consecutive failures — resetting session",
                     s_consec_failures);
            session_close();
            s_consec_failures = 0;
        }
        return rc;
    }

    s_consec_failures = 0;
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
        // client_id override (allow setting to empty string explicitly via set_cfg).
        if (over->client_id[0]) {
            strncpy(s_cfg.client_id, over->client_id, sizeof(s_cfg.client_id) - 1);
            s_cfg.client_id[sizeof(s_cfg.client_id) - 1] = '\0';
        }
        // headers override: only when num_headers > 0.
        if (over->num_headers > 0) {
            int n = over->num_headers;
            if (n > BB_SINK_HTTP_HEADERS_MAX) n = BB_SINK_HTTP_HEADERS_MAX;
            memcpy(s_cfg.headers, over->headers, (size_t)n * sizeof(bb_sink_http_header_t));
            s_cfg.num_headers = n;
        }
    }

    s_initialized = true;
    bb_log_i(TAG, "init: base=%s enabled=%d qos=%d headers=%d",
             s_cfg.base, s_cfg.enabled, s_cfg.qos, s_cfg.num_headers);
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
    out->publish   = http_pub_publish;
    out->ctx       = NULL;
    out->transport = "http";
    // tls reflects whether the configured base URL uses "https://".
    // Determined at wire time (bb_sink_http call) rather than per-publish;
    // transport config is boot-fixed under mutual-exclusion + reboot-to-switch.
    out->tls = (strncmp(s_cfg.base, "https://", 8) == 0);
    return BB_OK;
}
