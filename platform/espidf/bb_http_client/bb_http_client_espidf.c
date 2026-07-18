// ESP-IDF port for bb_http_client — wraps esp_http_client_perform with the
// retry / TLS-bundle pattern proven in bb_ota_pull.
#include "bb_http_client.h"
#include "bb_http_client_health.h"  // PRIV_INCLUDE_DIRS "src" (B1-1041 per-session health)
#include "bb_log.h"
#include "bb_mem.h"

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
#ifndef CONFIG_BB_HTTP_CLIENT_BACKOFF_BASE_MS
#define CONFIG_BB_HTTP_CLIENT_BACKOFF_BASE_MS 2000
#endif
#ifndef CONFIG_BB_HTTP_CLIENT_BACKOFF_MAX_MS
#define CONFIG_BB_HTTP_CLIENT_BACKOFF_MAX_MS 8000
#endif
#ifndef CONFIG_BB_HTTP_CLIENT_STREAM_CHUNK_BYTES
#define CONFIG_BB_HTTP_CLIENT_STREAM_CHUNK_BYTES 2048
#endif
#ifndef CONFIG_BB_HTTP_CLIENT_DEFAULT_BUF_SIZE
#define CONFIG_BB_HTTP_CLIENT_DEFAULT_BUF_SIZE 4096
#endif

// Exponential back-off: base * 2^attempt, capped at max.
static void apply_backoff(int attempt)
{
    int delay_ms = CONFIG_BB_HTTP_CLIENT_BACKOFF_BASE_MS;
    for (int i = 0; i < attempt; i++) {
        delay_ms *= 2;
        if (delay_ms >= CONFIG_BB_HTTP_CLIENT_BACKOFF_MAX_MS) {
            delay_ms = CONFIG_BB_HTTP_CLIENT_BACKOFF_MAX_MS;
            break;
        }
    }
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
}

bb_err_t bb_http_client_get(const char *url,
                            char *body, size_t body_cap,
                            const bb_http_client_cfg_t *cfg,
                            bb_http_client_result_t *out)
{
    if (!url || !body || body_cap == 0 || !out) return BB_ERR_INVALID_ARG;

    uint32_t timeout_ms = (cfg && cfg->timeout_ms)   ? cfg->timeout_ms   : CONFIG_BB_HTTP_CLIENT_DEFAULT_TIMEOUT_MS;
    uint8_t  attempts   = (cfg && cfg->max_attempts) ? cfg->max_attempts : CONFIG_BB_HTTP_CLIENT_DEFAULT_MAX_ATTEMPTS;
    uint16_t buf_size   = (cfg && cfg->buffer_size)  ? cfg->buffer_size  : CONFIG_BB_HTTP_CLIENT_DEFAULT_BUF_SIZE;
    const char *ua      = (cfg && cfg->user_agent)   ? cfg->user_agent   : "bb_http_client/0.1";
    const char *accept  = (cfg && cfg->accept_header)? cfg->accept_header: "*/*";

    out->status_code = 0;
    out->body_len = 0;
    out->truncated = false;
    out->tls_error_code = 0;

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
    uint16_t buf_size   = (cfg && cfg->buffer_size)  ? cfg->buffer_size  : CONFIG_BB_HTTP_CLIENT_DEFAULT_BUF_SIZE;
    const char *ua      = (cfg && cfg->user_agent)   ? cfg->user_agent   : "bb_http_client/0.1";
    const char *accept  = (cfg && cfg->accept_header)? cfg->accept_header: "*/*";

    out->status_code = 0;
    out->body_len = 0;
    out->truncated = false;
    out->tls_error_code = 0;

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

    // Stream body in chunks to the callback.
    char chunk_buf[CONFIG_BB_HTTP_CLIENT_STREAM_CHUNK_BYTES];
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
    uint16_t buf_size   = (cfg && cfg->buffer_size)  ? cfg->buffer_size  : CONFIG_BB_HTTP_CLIENT_DEFAULT_BUF_SIZE;
    const char *ua      = (cfg && cfg->user_agent)   ? cfg->user_agent   : "bb_http_client/0.1";
    const char *ct      = content_type               ? content_type      : "application/json";

    out->status_code = 0;
    out->body_len = 0;
    out->truncated = false;
    out->tls_error_code = 0;

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
    bb_http_client_health_state_t health;  // B1-1041: per-session health
} espidf_session_t;

bb_err_t bb_http_client_session_open(const bb_http_client_cfg_t *cfg,
                                     const char *url_base,
                                     bb_http_client_session_t *out)
{
    if (!url_base || !out) return BB_ERR_INVALID_ARG;

    uint32_t timeout_ms = (cfg && cfg->timeout_ms)   ? cfg->timeout_ms   : CONFIG_BB_HTTP_CLIENT_DEFAULT_TIMEOUT_MS;
    uint16_t buf_size   = (cfg && cfg->buffer_size)  ? cfg->buffer_size  : CONFIG_BB_HTTP_CLIENT_DEFAULT_BUF_SIZE;
    const char *ua      = (cfg && cfg->user_agent)   ? cfg->user_agent   : "bb_http_client/0.1";
    const char *ca_pem  = (cfg && cfg->ca_cert_pem)  ? cfg->ca_cert_pem  : NULL;

    espidf_session_t *s = (espidf_session_t *)bb_calloc_prefer_spiram(1, sizeof(espidf_session_t));
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
        bb_mem_free(s);
        return BB_ERR_INVALID_STATE;
    }

    pthread_mutex_init(&s->health.lock, NULL);  // B1-1041

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
        int tls_code = 0;
        esp_http_client_get_and_clear_last_tls_error(s->client, &tls_code, NULL);
        esp_http_client_close(s->client);
        out->status_code  = 0;
        out->body_len     = 0;
        out->truncated    = false;
        out->tls_error_code = tls_code;
        // B1-1041: transport failure -- connected=false, fail_count++.
        bb_http_client_priv_health_report(&s->health, false, 0);
        if (tls_code != 0) {
            bb_http_client_priv_health_set_tls_error(&s->health, tls_code);
        }
        return BB_ERR_INVALID_STATE;
    }

    int status = esp_http_client_get_status_code(s->client);
    out->status_code = status;
    out->body_len    = 0;
    out->truncated   = false;
    out->tls_error_code = 0;
    // B1-1041: round trip completed -- connected=true, last_ok_ms stamped;
    // status_code >= 500 also bumps fail_count (see the reporting policy in
    // bb_http_client.h). tls_error_code is left untouched (0 here is not a
    // real error to record, not a reset).
    bb_http_client_priv_health_report(&s->health, true, status);

    if (status < 200 || status >= 300) {
        bb_log_w(TAG, "session POST %s: HTTP %d", url, status);
        return BB_ERR_INVALID_STATE;
    }

    bb_log_d(TAG, "session POST %s -> %d", url, status);
    return BB_OK;
}

bb_err_t bb_http_client_session_health_fill(bb_http_client_session_t sess,
                                            bb_http_client_session_health_snap_t *out)
{
    if (!sess || !out) return BB_ERR_INVALID_ARG;
    espidf_session_t *s = (espidf_session_t *)sess;
    pthread_mutex_lock(&s->health.lock);
    out->connected      = s->health.connected;
    out->last_ok_ms     = s->health.last_ok_ms;
    out->fail_count     = (uint64_t)s->health.fail_count;
    out->tls_error_code = s->health.tls_error_code;
    pthread_mutex_unlock(&s->health.lock);
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
    bb_http_client_priv_health_close(&s->health);  // B1-1041: clean close, fail_count untouched
    pthread_mutex_destroy(&s->health.lock);
    bb_mem_free(s);
}
