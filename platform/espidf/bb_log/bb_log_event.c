// bb_log_event — ESP-IDF only: "log" bb_event stream topic.
// Forwards every log line from s_log_vprintf (via its dedicated queue) as
// structured JSON on the "log" bb_event topic, the primary log transport
// served at GET /api/events?topic=log. The legacy /api/logs route is retired.
//
// Design: s_log_vprintf has its own event queue (depth BB_LOG_EVENT_QUEUE_LEN);
// this keeps the hot logging path non-blocking (drop-on-full with counter).
// The s_rb ringbuf and bb_diag tap slot are left untouched.

#ifdef ESP_PLATFORM

#include "../../host/bb_log/bb_log_event_parse.h"
#include "../../../components/bb_log/src/bb_log_internal.h"
#include "bb_log.h"
#include "bb_event.h"
#include "bb_event_routes.h"
#include "bb_init.h"
#include "bb_json.h"
#include "bb_clock.h"
#include "bb_openapi.h"
#include "bb_task_registry.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>
#include <inttypes.h>

#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#ifdef CONFIG_BB_LOG_EVENT_QUEUE_LEN
#define BB_LOG_EVENT_QUEUE_LEN CONFIG_BB_LOG_EVENT_QUEUE_LEN
#endif
#endif
#ifndef BB_LOG_EVENT_QUEUE_LEN
#define BB_LOG_EVENT_QUEUE_LEN 48
#endif

#define LOG_EVENT_TASK_STACK   3072
#define LOG_EVENT_TASK_PRIO    1    /* same as console writer — very low */

#define LOG_STREAM_LINE_MAX    192  /* mirrors bb_log.c */

static const char *TAG = "bb_log_event";

typedef struct {
    char   line[LOG_STREAM_LINE_MAX];
    size_t len;
} log_event_msg_t;

static bb_event_topic_t s_log_topic = NULL;
static QueueHandle_t    s_q         = NULL;
static TaskHandle_t     s_task      = NULL;

// ---------------------------------------------------------------------------
// Forwarder task
// ---------------------------------------------------------------------------

static void s_forwarder_task(void *arg)
{
    (void)arg;
    log_event_msg_t msg;
    char level;
    char tag[48];
    char msgbuf[168]; /* 160 + some margin for safe_copy */

    for (;;) {
        if (xQueueReceive(s_q, &msg, portMAX_DELAY) != pdTRUE) continue;
        if (!s_log_topic) continue;

        bb_log_event_parse(msg.line, msg.len, &level, tag, sizeof(tag),
                           msgbuf, sizeof(msgbuf));

        uint64_t ts = bb_clock_now_ms64();

        bb_json_t obj = bb_json_obj_new();
        if (!obj) continue;

        char level_str[2] = { level, '\0' };
        bb_json_obj_set_int(obj, "ts", (int64_t)ts);
        bb_json_obj_set_string(obj, "level", level_str);
        bb_json_obj_set_string(obj, "tag", tag);
        bb_json_obj_set_string(obj, "msg", msgbuf);

        char *payload = bb_json_serialize(obj);
        bb_json_free(obj);
        if (!payload) continue;

        size_t plen = strlen(payload);
        bb_event_post(s_log_topic, 0, payload, plen + 1);
        bb_json_free_str(payload);
    }
}

// ---------------------------------------------------------------------------
// Auto-attach registry init (order 4, after bb_event_routes at order 0)
// ---------------------------------------------------------------------------

#if defined(CONFIG_BB_LOG_EVENT_AUTO_ATTACH) && CONFIG_BB_LOG_EVENT_AUTO_ATTACH

static const char k_log_event_schema[] =
    "{\"title\":\"LogEvent\",\"x-sse-topic\":\"log\",\"type\":\"object\","
    "\"properties\":{"
    "\"ts\":{\"type\":\"integer\"},"
    "\"level\":{\"type\":\"string\",\"enum\":[\"I\",\"W\",\"E\",\"D\",\"V\",\"?\"]},"
    "\"tag\":{\"type\":\"string\"},"
    "\"msg\":{\"type\":\"string\"}},"
    "\"required\":[\"ts\",\"level\",\"tag\",\"msg\"]}";

static bb_err_t bb_log_event_init(bb_http_handle_t server)
{
    (void)server;

    bb_err_t err = bb_event_topic_register("log", &s_log_topic);
    if (err != BB_OK) {
        bb_log_e(TAG, "topic register failed: %d", err);
        return err;
    }

    bb_openapi_register_topic_schema("log", k_log_event_schema, "LogEvent");

    err = bb_event_routes_attach_ex("log", /*retained=*/false);
    if (err != BB_OK) {
        bb_log_w(TAG, "routes attach failed: %d", err);
        /* non-fatal — topic still usable for direct subscribers */
    }

    s_q = xQueueCreate(BB_LOG_EVENT_QUEUE_LEN, sizeof(log_event_msg_t));
    if (!s_q) {
        bb_log_e(TAG, "queue alloc failed");
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(s_forwarder_task, "bb_log_evt", LOG_EVENT_TASK_STACK,
                    NULL, LOG_EVENT_TASK_PRIO, &s_task) != pdPASS) {
        vQueueDelete(s_q);
        s_q = NULL;
        bb_log_e(TAG, "task create failed");
        return ESP_ERR_NO_MEM;
    }
    bb_task_registry_register("bb_log_evt", LOG_EVENT_TASK_STACK, s_task, NULL, NULL);

    bb_log_event_set_queue(s_q);

    bb_log_i(TAG, "log event topic registered and attached");
    return BB_OK;
}

BB_INIT_REGISTER_N(bb_log_event, bb_log_event_init, 4)

#endif /* CONFIG_BB_LOG_EVENT_AUTO_ATTACH */

#endif /* ESP_PLATFORM */
