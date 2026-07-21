// bb_log_event — ESP-IDF only: "log" bb_data key. Forwards every log line
// from s_log_vprintf (via its dedicated queue) as structured JSON, stashing
// it for bb_log_event_gather() and bumping the "log" bb_data generation
// (B1-1045) -- the primary log transport, served at GET /api/events?topic=log
// via the composition root's bb_data_http attach. The legacy /api/logs route
// is retired.
//
// Design: s_log_vprintf has its own event queue (depth BB_LOG_EVENT_QUEUE_LEN);
// this keeps the hot logging path non-blocking (drop-on-full with counter).
// The s_rb ringbuf and bb_diag tap slot are left untouched.

#ifdef ESP_PLATFORM

#include "bb_log_event.h"
#include "bb_log_event_wire.h"
#include "../../../components/bb_log_event/bb_log_event_line_wire_priv.h"
#include "../../host/bb_log_event/bb_log_event_parse.h"
#include "../../../components/bb_log/src/bb_log_internal.h"
#include "bb_log.h"
#include "bb_data.h"
#include "bb_http_server.h"
#include "bb_serialize_json.h"
#include "bb_clock.h"
#include "bb_openapi.h"
#include "bb_task.h"
#include "bb_str.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <assert.h>
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
#define BB_LOG_EVENT_QUEUE_LEN 24
#endif

#define LOG_EVENT_TASK_STACK   3072
#define LOG_EVENT_TASK_PRIO    1    /* same as console writer — very low */

#define LOG_STREAM_LINE_MAX    192  /* mirrors bb_log.c */

static const char *TAG = "bb_log_event";

typedef struct {
    char   line[LOG_STREAM_LINE_MAX];
    size_t len;
} log_event_msg_t;

static QueueHandle_t    s_q         = NULL;
static TaskHandle_t     s_task      = NULL;

// B1-1045 PR-2 wire-primitive stash: the most recently forwarded "log"
// payload, for bb_log_event_gather() (bb_log_event_wire.h). Written ONLY in
// s_forwarder_task, immediately after the existing bb_event_post() call
// below -- a pure store, no new branch/early-return/lock/alloc, and
// s_forwarder_task's control flow (including the alloc-fail `continue`
// paths above) is unchanged.
static char s_last_log_json[BB_LOG_EVENT_LOG_TEXT_MAX];

// Render scratch for bb_serialize_json_render(), sized to
// BB_LOG_EVENT_LINE_JSON_MAX (bb_log_event_line_wire_priv.h) -- the
// descriptor's true worst case, so render can never return
// BB_ERR_NO_SPACE. FILE-SCOPE STATIC, not a stack local: at 1431 bytes it
// would eat ~47% of s_forwarder_task's LOG_EVENT_TASK_STACK (3072 bytes)
// on top of that task's existing locals (tag[48] + msgbuf[168] + the
// bb_log_event_line_wire_t snap, ~226B already) plus the
// bb_serialize_json_render()/bb_data_touch() call chain -- too little
// headroom. s_forwarder_task is the only reader/writer (single dedicated
// task, no reentrancy), same rationale as the existing static
// s_last_log_json stash above.
static char s_render_buf[BB_LOG_EVENT_LINE_JSON_MAX];

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

        bb_log_event_parse(msg.line, msg.len, &level, tag, sizeof(tag),
                           msgbuf, sizeof(msgbuf));

        uint64_t ts = bb_clock_now_ms64();

        bb_log_event_line_wire_t snap;
        memset(&snap, 0, sizeof(snap));
        // Exact-decimal int64 render (not cJSON's double-cast path) --
        // deliberate: identical digit output for realistic ms-epoch values,
        // with none of double's precision loss at large magnitudes.
        snap.ts = (int64_t)ts;
        char level_str[2] = { level, '\0' };
        bb_strlcpy(snap.level, level_str, sizeof(snap.level));
        bb_strlcpy(snap.tag, tag, sizeof(snap.tag));
        bb_strlcpy(snap.msg, msgbuf, sizeof(snap.msg));

        size_t out_len = 0;
        bb_err_t rc = bb_serialize_json_render(&bb_log_event_line_wire_desc, &snap,
                                                s_render_buf, sizeof(s_render_buf), &out_len);
        if (rc != BB_OK) continue;

        // B1-1045 PR-4: stash first, THEN bump the "log" bb_data generation
        // -- a consumer that observes the new generation must always see the
        // fresh stash, never a stale one (mirrors every other producer's
        // stash-then-touch ordering). bb_strlcpy truncates a line whose full
        // render exceeds the 220-byte stash -- parity with the old cJSON
        // path, which built the full string then truncated on copy; never
        // dropped.
        bb_strlcpy(s_last_log_json, s_render_buf, sizeof(s_last_log_json));
        bb_data_touch("log");
    }
}

// ---------------------------------------------------------------------------
// B1-1045 PR-2 wire-primitive gather -- ESP-IDF only, not host-reproducible
// ---------------------------------------------------------------------------

bb_err_t bb_log_event_gather(bb_log_event_wire_t *dst)
{
    if (!dst) return BB_ERR_INVALID_ARG;
    bb_strlcpy(dst->log, s_last_log_json, sizeof(dst->log));
    return BB_OK;
}

// ---------------------------------------------------------------------------
// Forwarder queue/task bring-up (B1-1045: no more bb_event topic/route
// registration -- the "log" key's bb_data binding + /api/events attach are
// composition-root concerns now, see examples/floor/main/floor_app.c)
// ---------------------------------------------------------------------------

static const char k_log_event_schema[] =
    "{\"title\":\"LogEvent\",\"x-sse-topic\":\"log\",\"type\":\"object\","
    "\"properties\":{"
    "\"ts\":{\"type\":\"integer\"},"
    "\"level\":{\"type\":\"string\",\"enum\":[\"I\",\"W\",\"E\",\"D\",\"V\",\"?\"]},"
    "\"tag\":{\"type\":\"string\"},"
    "\"msg\":{\"type\":\"string\"}},"
    "\"required\":[\"ts\",\"level\",\"tag\",\"msg\"]}";

bb_err_t bb_log_event_init(bb_http_handle_t server)
{
    (void)server;

    // Belt-and-suspenders: s_render_buf must never be smaller than the
    // descriptor's true worst case, or bb_serialize_json_render() could
    // return BB_ERR_NO_SPACE and silently drop a line again. Runtime
    // assert, not _Static_assert -- bb_serialize_json_bound() walks the
    // descriptor's fields at runtime, not a compile-time constant.
    assert(sizeof(s_render_buf) >= bb_serialize_json_bound(&bb_log_event_line_wire_desc));

    bb_openapi_register_topic_schema("log", k_log_event_schema, "LogEvent");

    s_q = xQueueCreate(BB_LOG_EVENT_QUEUE_LEN, sizeof(log_event_msg_t));
    if (!s_q) {
        bb_log_e(TAG, "queue alloc failed");
        return ESP_ERR_NO_MEM;
    }

    bb_task_config_t log_evt_cfg = {
        .entry       = s_forwarder_task,
        .name        = "bb_log_evt",
        .arg         = NULL,
        .stack_bytes = LOG_EVENT_TASK_STACK,
        .priority    = LOG_EVENT_TASK_PRIO,
        .core        = BB_TASK_CORE_ANY,
        .backing     = BB_TASK_BACKING_DYNAMIC,
        .wdt_arm     = false,
    };
    if (bb_task_create(&log_evt_cfg, (void **)&s_task) != BB_OK) {
        vQueueDelete(s_q);
        s_q = NULL;
        bb_log_e(TAG, "task create failed");
        return ESP_ERR_NO_MEM;
    }

    bb_log_event_set_queue(s_q);

    bb_log_i(TAG, "log event forwarder started");
    return BB_OK;
}

#endif /* ESP_PLATFORM */
