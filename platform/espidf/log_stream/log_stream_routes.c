#include "log_stream.h"
#include "http_server.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bb_log_routes";

static volatile TaskHandle_t s_sse_task_handle = NULL;
static volatile int s_sse_client_type = 0;  // 0=none, 1=browser, 2=external
static volatile bool s_sse_stop = false;

static void sse_task(void *arg)
{
    httpd_req_t *req = (httpd_req_t *)arg;

    int fd = httpd_req_to_sockfd(req);
    struct timeval tv = { .tv_sec = 30, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    httpd_resp_set_type(req, "text/event-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "Connection", "keep-alive");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Private-Network", "true");

    esp_err_t err = httpd_resp_send_chunk(req, ": connected\n\n", HTTPD_RESP_USE_STRLEN);

    char line[192];
    char frame[220];
    while (err == ESP_OK && !s_sse_stop) {
        size_t n = bb_log_stream_drain(line, sizeof(line), 500);
        if (n == 0) continue;
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = '\0';
        int flen = snprintf(frame, sizeof(frame), "data: %s\n\n", line);
        err = httpd_resp_send_chunk(req, frame,
                    (flen > 0 && flen < (int)sizeof(frame)) ? flen : HTTPD_RESP_USE_STRLEN);
    }

    httpd_resp_send_chunk(req, NULL, 0);
    httpd_req_async_handler_complete(req);
    s_sse_task_handle = NULL;
    s_sse_client_type = 0;
    vTaskDelete(NULL);
}

static esp_err_t logs_handler(httpd_req_t *req)
{
    int client_type = 2;
    char query[32];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char val[16];
        if (httpd_query_key_value(query, "source", val, sizeof(val)) == ESP_OK
            && strcmp(val, "browser") == 0) {
            client_type = 1;
        }
    }

    if (s_sse_task_handle && !(client_type == 2 && s_sse_client_type == 1)) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, "Log stream in use");
        return ESP_OK;
    }

    if (s_sse_task_handle) {
        s_sse_stop = true;
        for (int i = 0; i < 10 && s_sse_task_handle; i++)
            vTaskDelay(pdMS_TO_TICKS(100));
    }
    s_sse_stop = false;

    if (client_type == 2) {
        ESP_LOGI(TAG, "external log client connected");
    }

    httpd_req_t *async_req = NULL;
    if (httpd_req_async_handler_begin(req, &async_req) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Async init failed");
        return ESP_FAIL;
    }

    s_sse_client_type = client_type;
    if (xTaskCreate(sse_task, "sse_log", 4096, async_req, 1, (TaskHandle_t *)&s_sse_task_handle) != pdPASS) {
        httpd_req_async_handler_complete(async_req);
        s_sse_client_type = 0;
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t logs_status_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    char buf[96];
    uint32_t dropped = bb_log_stream_dropped_lines();
    if (s_sse_client_type == 0) {
        snprintf(buf, sizeof(buf), "{\"active\":false,\"client\":null,\"dropped\":%" PRIu32 "}", dropped);
    } else {
        snprintf(buf, sizeof(buf), "{\"active\":true,\"client\":\"%s\",\"dropped\":%" PRIu32 "}",
                 s_sse_client_type == 1 ? "browser" : "external", dropped);
    }
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

esp_err_t bb_log_stream_register_routes(void *server)
{
    if (!server) return ESP_ERR_INVALID_ARG;
    httpd_handle_t h = (httpd_handle_t)server;

    httpd_uri_t logs_uri = {
        .uri = "/api/logs", .method = HTTP_GET, .handler = logs_handler, .user_ctx = NULL,
    };
    httpd_uri_t logs_status_uri = {
        .uri = "/api/logs/status", .method = HTTP_GET, .handler = logs_status_handler, .user_ctx = NULL,
    };

    esp_err_t err = httpd_register_uri_handler(h, &logs_uri);
    if (err != ESP_OK) return err;
    return httpd_register_uri_handler(h, &logs_status_uri);
}
