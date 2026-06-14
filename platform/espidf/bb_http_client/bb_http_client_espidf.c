// ESP-IDF port for bb_http_client — wraps esp_http_client_perform with the
// retry / TLS-bundle pattern proven in bb_ota_pull.
#include "bb_http_client.h"
#include "bb_log.h"

#include <stdbool.h>
#include <string.h>

#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bb_http_client";

#ifndef CONFIG_BB_HTTP_CLIENT_DEFAULT_TIMEOUT_MS
#define CONFIG_BB_HTTP_CLIENT_DEFAULT_TIMEOUT_MS 10000
#endif
#ifndef CONFIG_BB_HTTP_CLIENT_DEFAULT_MAX_ATTEMPTS
#define CONFIG_BB_HTTP_CLIENT_DEFAULT_MAX_ATTEMPTS 3
#endif

static const int s_backoff_ms[] = {2000, 4000, 8000};

static void apply_backoff(int attempt)
{
    int len = (int)(sizeof(s_backoff_ms) / sizeof(s_backoff_ms[0]));
    int idx = attempt < len ? attempt : len - 1;
    vTaskDelay(pdMS_TO_TICKS(s_backoff_ms[idx]));
}

bb_err_t bb_http_client_get(const char *url,
                            char *body, size_t body_cap,
                            const bb_http_client_cfg_t *cfg,
                            bb_http_client_result_t *out)
{
    if (!url || !body || body_cap == 0 || !out) return BB_ERR_INVALID_ARG;

    uint32_t timeout_ms = (cfg && cfg->timeout_ms)   ? cfg->timeout_ms   : CONFIG_BB_HTTP_CLIENT_DEFAULT_TIMEOUT_MS;
    uint8_t  attempts   = (cfg && cfg->max_attempts) ? cfg->max_attempts : CONFIG_BB_HTTP_CLIENT_DEFAULT_MAX_ATTEMPTS;
    uint16_t buf_size   = (cfg && cfg->buffer_size)  ? cfg->buffer_size  : 4096;
    const char *ua      = (cfg && cfg->user_agent)   ? cfg->user_agent   : "bb_http_client/0.1";
    const char *accept  = (cfg && cfg->accept_header)? cfg->accept_header: "*/*";

    out->status_code = 0;
    out->body_len = 0;
    out->truncated = false;

    esp_http_client_config_t config = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = (int)timeout_ms,
        .user_agent = ua,
        .buffer_size = buf_size,
    };

    esp_http_client_handle_t client = NULL;
    esp_err_t err = ESP_FAIL;
    int status = 0;

    for (int attempt = 0; attempt < attempts; attempt++) {
        client = esp_http_client_init(&config);
        if (!client) {
            err = ESP_FAIL;
            bb_log_w(TAG, "init attempt %d/%d failed", attempt + 1, attempts);
        } else {
            esp_http_client_set_header(client, "Accept", accept);
            esp_http_client_set_method(client, HTTP_METHOD_GET);
            err = esp_http_client_open(client, 0);
            if (err == ESP_OK) {
                esp_http_client_fetch_headers(client);
                status = esp_http_client_get_status_code(client);
                if (status >= 200 && status < 600) {
                    break;  // any HTTP response counts as transport success
                }
                err = ESP_FAIL;
            }
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            client = NULL;
        }
        if (attempt + 1 < attempts) {
            apply_backoff(attempt);
        }
    }

    if (err != ESP_OK || !client) {
        bb_log_e(TAG, "%s: transport failed after %d attempts", url, attempts);
        return BB_ERR_INVALID_STATE;
    }

    // Drain body into caller buffer.
    size_t total = 0;
    int read_len;
    bool truncated = false;
    while (total + 1 < body_cap) {
        read_len = esp_http_client_read(client, body + total, body_cap - total - 1);
        if (read_len <= 0) break;
        total += (size_t)read_len;
    }
    // Detect truncation by attempting one more read; if it returns bytes,
    // the server had more to give than we could hold.
    if (total + 1 >= body_cap) {
        char overflow[16];
        if (esp_http_client_read(client, overflow, sizeof(overflow)) > 0) {
            truncated = true;
        }
    }
    body[total] = '\0';

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    out->status_code = status;
    out->body_len = total;
    out->truncated = truncated;
    return BB_OK;
}

bb_err_t bb_http_client_get_stream(const char *url,
                                   bb_http_client_chunk_cb cb, void *ctx,
                                   const bb_http_client_cfg_t *cfg,
                                   bb_http_client_result_t *out)
{
    if (!url || !cb || !out) return BB_ERR_INVALID_ARG;

    uint32_t timeout_ms = (cfg && cfg->timeout_ms)   ? cfg->timeout_ms   : CONFIG_BB_HTTP_CLIENT_DEFAULT_TIMEOUT_MS;
    uint8_t  attempts   = (cfg && cfg->max_attempts) ? cfg->max_attempts : CONFIG_BB_HTTP_CLIENT_DEFAULT_MAX_ATTEMPTS;
    uint16_t buf_size   = (cfg && cfg->buffer_size)  ? cfg->buffer_size  : 4096;
    const char *ua      = (cfg && cfg->user_agent)   ? cfg->user_agent   : "bb_http_client/0.1";
    const char *accept  = (cfg && cfg->accept_header)? cfg->accept_header: "*/*";

    out->status_code = 0;
    out->body_len = 0;
    out->truncated = false;

    esp_http_client_config_t config = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = (int)timeout_ms,
        .user_agent = ua,
        .buffer_size = buf_size,
    };

    esp_http_client_handle_t client = NULL;
    esp_err_t err = ESP_FAIL;
    int status = 0;

    for (int attempt = 0; attempt < attempts; attempt++) {
        client = esp_http_client_init(&config);
        if (!client) {
            err = ESP_FAIL;
            bb_log_w(TAG, "stream init attempt %d/%d failed", attempt + 1, attempts);
        } else {
            esp_http_client_set_header(client, "Accept", accept);
            esp_http_client_set_method(client, HTTP_METHOD_GET);
            err = esp_http_client_open(client, 0);
            if (err == ESP_OK) {
                esp_http_client_fetch_headers(client);
                status = esp_http_client_get_status_code(client);
                if (status >= 200 && status < 600) {
                    break;
                }
                err = ESP_FAIL;
            }
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            client = NULL;
        }
        if (attempt + 1 < attempts) {
            apply_backoff(attempt);
        }
    }

    if (err != ESP_OK || !client) {
        bb_log_e(TAG, "%s: stream transport failed after %d attempts", url, attempts);
        return BB_ERR_INVALID_STATE;
    }

    // Stream body in 2 KB chunks to the callback.
    char chunk_buf[2048];
    size_t total = 0;
    int read_len;
    bb_err_t cb_err = BB_OK;

    while (cb_err == BB_OK) {
        read_len = esp_http_client_read(client, chunk_buf, sizeof(chunk_buf));
        if (read_len <= 0) break;
        total += (size_t)read_len;
        cb_err = cb(ctx, chunk_buf, (size_t)read_len);
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    out->status_code = status;
    out->body_len = total;
    out->truncated = (cb_err == BB_ERR_NO_SPACE);
    if (cb_err != BB_OK) return cb_err;
    return BB_OK;
}

bb_err_t bb_http_client_post(const char *url,
                             const char *body, size_t body_len,
                             const char *content_type,
                             char *resp, size_t resp_cap,
                             const bb_http_client_cfg_t *cfg,
                             bb_http_client_result_t *out)
{
    if (!url || !resp || resp_cap == 0 || !out) return BB_ERR_INVALID_ARG;

    uint32_t timeout_ms = (cfg && cfg->timeout_ms)   ? cfg->timeout_ms   : CONFIG_BB_HTTP_CLIENT_DEFAULT_TIMEOUT_MS;
    uint8_t  attempts   = (cfg && cfg->max_attempts) ? cfg->max_attempts : CONFIG_BB_HTTP_CLIENT_DEFAULT_MAX_ATTEMPTS;
    uint16_t buf_size   = (cfg && cfg->buffer_size)  ? cfg->buffer_size  : 4096;
    const char *ua      = (cfg && cfg->user_agent)   ? cfg->user_agent   : "bb_http_client/0.1";
    const char *ct      = content_type               ? content_type      : "application/json";

    out->status_code = 0;
    out->body_len = 0;
    out->truncated = false;

    // Use ca_cert_pem override when provided; otherwise fall back to bundle.
    const char *ca_pem = (cfg && cfg->ca_cert_pem) ? cfg->ca_cert_pem : NULL;

    esp_http_client_config_t config = {
        .url              = url,
        .crt_bundle_attach = (ca_pem == NULL) ? esp_crt_bundle_attach : NULL,
        .cert_pem         = ca_pem,
        .client_cert_pem  = (cfg && cfg->client_cert_pem) ? cfg->client_cert_pem : NULL,
        .client_key_pem   = (cfg && cfg->client_key_pem)  ? cfg->client_key_pem  : NULL,
        .timeout_ms       = (int)timeout_ms,
        .user_agent       = ua,
        .buffer_size      = buf_size,
        .method           = HTTP_METHOD_POST,
    };

    esp_http_client_handle_t client = NULL;
    esp_err_t err = ESP_FAIL;
    int status = 0;

    for (int attempt = 0; attempt < attempts; attempt++) {
        client = esp_http_client_init(&config);
        if (!client) {
            err = ESP_FAIL;
            bb_log_w(TAG, "post init attempt %d/%d failed", attempt + 1, attempts);
        } else {
            esp_http_client_set_method(client, HTTP_METHOD_POST);
            esp_http_client_set_header(client, "Content-Type", ct);
            // Manual open flow: the body MUST be written explicitly with
            // esp_http_client_write(). esp_http_client_set_post_field() only
            // applies to esp_http_client_perform(), not open/write — without the
            // write the server blocks waiting for the Content-Length body that
            // never arrives, so fetch_headers hangs until the request times out.
            err = esp_http_client_open(client, (int)body_len);
            if (err == ESP_OK) {
                int wlen = (body && body_len > 0)
                    ? esp_http_client_write(client, body, (int)body_len)
                    : 0;
                if (wlen >= 0) {
                    esp_http_client_fetch_headers(client);
                    status = esp_http_client_get_status_code(client);
                    if (status >= 200 && status < 600) {
                        break;
                    }
                }
                err = ESP_FAIL;
            }
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            client = NULL;
        }
        if (attempt + 1 < attempts) {
            apply_backoff(attempt);
        }
    }

    if (err != ESP_OK || !client) {
        bb_log_e(TAG, "%s: post transport failed after %d attempts", url, attempts);
        return BB_ERR_INVALID_STATE;
    }

    // Drain response body into caller buffer.
    size_t total = 0;
    int read_len;
    bool truncated = false;
    while (total + 1 < resp_cap) {
        read_len = esp_http_client_read(client, resp + total, resp_cap - total - 1);
        if (read_len <= 0) break;
        total += (size_t)read_len;
    }
    if (total + 1 >= resp_cap) {
        char overflow[16];
        if (esp_http_client_read(client, overflow, sizeof(overflow)) > 0) {
            truncated = true;
        }
    }
    resp[total] = '\0';

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    out->status_code = status;
    out->body_len    = total;
    out->truncated   = truncated;
    return BB_OK;
}

// ---------------------------------------------------------------------------
// Reusable keep-alive session
// ---------------------------------------------------------------------------

typedef struct {
    esp_http_client_handle_t client;
} espidf_session_t;

bb_err_t bb_http_client_session_open(const bb_http_client_cfg_t *cfg,
                                     const char *url_base,
                                     bb_http_client_session_t *out)
{
    if (!url_base || !out) return BB_ERR_INVALID_ARG;

    uint32_t timeout_ms = (cfg && cfg->timeout_ms)   ? cfg->timeout_ms   : CONFIG_BB_HTTP_CLIENT_DEFAULT_TIMEOUT_MS;
    uint16_t buf_size   = (cfg && cfg->buffer_size)  ? cfg->buffer_size  : 4096;
    const char *ua      = (cfg && cfg->user_agent)   ? cfg->user_agent   : "bb_http_client/0.1";
    const char *ca_pem  = (cfg && cfg->ca_cert_pem)  ? cfg->ca_cert_pem  : NULL;

    espidf_session_t *s = (espidf_session_t *)calloc(1, sizeof(espidf_session_t));
    if (!s) return BB_ERR_NO_SPACE;

    bool keep_alive = (cfg && cfg->keep_alive);

    esp_http_client_config_t config = {
        .url                = url_base,
        .method             = HTTP_METHOD_POST,
        .keep_alive_enable  = keep_alive,
        .crt_bundle_attach  = (ca_pem == NULL) ? esp_crt_bundle_attach : NULL,
        .cert_pem           = ca_pem,
        .client_cert_pem    = (cfg && cfg->client_cert_pem) ? cfg->client_cert_pem : NULL,
        .client_key_pem     = (cfg && cfg->client_key_pem)  ? cfg->client_key_pem  : NULL,
        .timeout_ms         = (int)timeout_ms,
        .user_agent         = ua,
        .buffer_size        = buf_size,
    };

    s->client = esp_http_client_init(&config);
    if (!s->client) {
        free(s);
        return BB_ERR_INVALID_STATE;
    }

    *out = (bb_http_client_session_t)s;
    return BB_OK;
}

bb_err_t bb_http_client_session_post(bb_http_client_session_t sess,
                                     const char *url,
                                     const char *body, size_t body_len,
                                     const char *content_type,
                                     bb_http_client_result_t *out)
{
    if (!sess || !url || !out) return BB_ERR_INVALID_ARG;
    espidf_session_t *s = (espidf_session_t *)sess;

    const char *ct = content_type ? content_type : "application/json";

    esp_http_client_set_url(s->client, url);
    esp_http_client_set_header(s->client, "Content-Type", ct);
    esp_http_client_set_post_field(s->client, body, (int)body_len);

    esp_err_t err = esp_http_client_perform(s->client);
    if (err != ESP_OK) {
        bb_log_w(TAG, "session POST %s: perform error %d — closing socket", url, err);
        esp_http_client_close(s->client);
        out->status_code = 0;
        out->body_len    = 0;
        out->truncated   = false;
        return BB_ERR_INVALID_STATE;
    }

    int status = esp_http_client_get_status_code(s->client);
    out->status_code = status;
    out->body_len    = 0;
    out->truncated   = false;

    if (status < 200 || status >= 300) {
        bb_log_w(TAG, "session POST %s: HTTP %d", url, status);
        return BB_ERR_INVALID_STATE;
    }

    bb_log_d(TAG, "session POST %s -> %d", url, status);
    return BB_OK;
}

bb_err_t bb_http_client_session_set_header(bb_http_client_session_t sess,
                                           const char *name,
                                           const char *value)
{
    if (!sess || !name || !value) return BB_ERR_INVALID_ARG;
    espidf_session_t *s = (espidf_session_t *)sess;
    esp_err_t err = esp_http_client_set_header(s->client, name, value);
    return (err == ESP_OK) ? BB_OK : BB_ERR_INVALID_STATE;
}

void bb_http_client_session_close(bb_http_client_session_t sess)
{
    if (!sess) return;
    espidf_session_t *s = (espidf_session_t *)sess;
    if (s->client) {
        esp_http_client_cleanup(s->client);
        s->client = NULL;
    }
    free(s);
}
