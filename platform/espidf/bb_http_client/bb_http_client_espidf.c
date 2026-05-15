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
            int idx = attempt < (int)(sizeof(s_backoff_ms)/sizeof(s_backoff_ms[0]))
                          ? attempt : (int)(sizeof(s_backoff_ms)/sizeof(s_backoff_ms[0])) - 1;
            vTaskDelay(pdMS_TO_TICKS(s_backoff_ms[idx]));
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
