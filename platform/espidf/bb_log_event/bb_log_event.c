// bb_log_event — ESP-IDF only: "log" bb_data key. Forwards every log line
// from s_log_vprintf (via its dedicated queue) as structured JSON, stashing
// it for bb_log_event_gather() and bumping the "log" bb_data generation
// (B1-1045) -- the primary log transport, served at GET /api/events?topic=log
// via the composition root's bb_data_http attach. The legacy /api/logs route
// is retired.
//
// Design: s_log_vprintf has its own event queue (depth BB_LOG_EVENT_QUEUE_LEN);
// this keeps the hot logging path non-blocking (drop-on-full with counter).
// The bb_diag tap slot is left untouched.

#ifdef ESP_PLATFORM

#include "bb_log_event.h"
#include "bb_log_event_wire.h"
#include "../../../components/bb_log_event/bb_log_event_line_wire_priv.h"
#include "../../../components/bb_log/src/bb_log_internal.h"
#include "bb_log.h"
#include "bb_data.h"
#include "bb_http_server.h"
#include "bb_serialize_json.h"
#include "bb_clock.h"
#include "bb_data_http.h"
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

static const char *TAG = "bb_log_event";

// B1-1443 PR-1: the forwarder queue's item type is now the single
// bb_log_internal.h-shared bb_log_event_msg_t (both s_log_vprintf and
// bb_log_emit in platform/espidf/bb_log/bb_log.c enqueue onto it) -- see
// that type's doc comment for the structured/foreign split. Replaces the
// former private log_event_msg_t, which relied on a struct-prefix layout
// match with bb_log.c's log_writer_msg_t rather than a shared type.
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
    bb_log_event_msg_t msg;
    char level;
    char tag[BB_LOG_EVENT_MSG_TAG_MAX];
    char msgbuf[168]; /* 160 + some margin for safe_copy */
    uint64_t ts;

    for (;;) {
        if (xQueueReceive(s_q, &msg, portMAX_DELAY) != pdTRUE) continue;

        if (msg.structured) {
            // B1-1443 PR-1: this line came from bb_log_emit() (our own
            // bb_log_e/w/i/d/v calls) -- level/tag/msg/ts are the real
            // fields the call site already had; no re-parse needed.
            level = msg.level;
            bb_strlcpy(tag, msg.tag, sizeof(tag));
            bb_strlcpy(msgbuf, msg.line, sizeof(msgbuf));
            ts = msg.ts_ms;
        } else {
            // Foreign/vendored ESP_LOGx output -- unchanged from before
            // this PR: parse the already-formatted console text, and stamp
            // ts at drain time (this path's own fidelity gap is PR-2 scope,
            // not this one).
            bb_log_line_parse(msg.line, msg.len, &level, tag, sizeof(tag),
                              msgbuf, sizeof(msgbuf));
            ts = bb_clock_now_ms64();
        }

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
        bb_serialize_json_render_cfg_t render_cfg = {
            .desc = &bb_log_event_line_wire_desc,
            .snap = &snap,
            .buf  = s_render_buf,
            .cap  = sizeof(s_render_buf),
        };
        bb_err_t rc = bb_serialize_json_render(&render_cfg, &out_len);
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

// SSE topic schema for "log" (B1-1059 SSE batch PR-3): the hand literal
// moved to bb_log_event_line_wire.c (relocation, see its own banner) --
// config-OFF this register call serves that literal unchanged; config-ON,
// the schema is composed first, before serving the runtime-composed
// buffer -- a compose failure is degrade-and-continue (warn, keep going),
// not fail-loud, see the comment at the call site below.
//
// B1-1220 PR2 (the pilot migration): describes via bb_data_http_describe()
// rather than the legacy bb_openapi_register_topic_schema() -- bb_log_event
// no longer links bb_openapi at all (see CMakeLists.txt). key and topic are
// both "log" (the bb_data key this producer binds under is also the /api/
// events SSE topic name, see examples/floor/main/floor_app.c's producers[]
// bind loop). A composition root that wants "log" back in /api/openapi.json
// must wire bb_openapi_set_topic_source_fn(bb_data_http_describe_foreach) --
// examples/smoke now wires this seam (see its entry_espidf.c) -- see
// bb_openapi.h's seam doc.
bb_err_t bb_log_event_init(bb_http_handle_t server)
{
    (void)server;

    // Belt-and-suspenders: s_render_buf must never be smaller than the
    // descriptor's true worst case, or bb_serialize_json_render() could
    // return BB_ERR_NO_SPACE and silently drop a line again. Runtime
    // assert, not _Static_assert -- bb_serialize_json_bound() walks the
    // descriptor's fields at runtime, not a compile-time constant.
    assert(sizeof(s_render_buf) >= bb_serialize_json_bound(&bb_log_event_line_wire_desc));

    // Doc-only bookkeeping (feeds /api/openapi.json schema synthesis) --
    // a compose failure here must not abort bring-up: schema composition
    // is documentation-only and must never take down logging. Degrade
    // and continue -- log a warning and fall through so the queue/task/
    // subscription below still comes up. But a compose failure must
    // degrade to "no LogEvent entry in the document", never "an invalid
    // entry that poisons the whole document": on failure,
    // bb_log_event_line_ensure_schema_patched() guarantees the schema
    // buffer is left EMPTY, and bb_openapi_register_schema() rejects only
    // a NULL literal, not "" -- an empty literal would still register and
    // later get spliced raw into the JSON document as `"LogEvent":` with
    // no value, corrupting every topic's entry, not just log's. So skip
    // registration entirely when compose failed.
#if defined(CONFIG_BB_OPENAPI_RUNTIME_META)
    bb_err_t schema_rc = bb_log_event_line_ensure_schema_patched();
    if (schema_rc != BB_OK) {
        bb_log_w(TAG, "log schema compose failed: %d", (int)schema_rc);
    } else {
        // Doc-only bookkeeping (feeds /api/openapi.json schema synthesis, see
        // bb_openapi.h's topic-source seam doc) -- this call is new with the
        // migration, so it is born correct: a describe failure must not abort
        // bring-up, since bb_data_http_describe()'s backing table
        // (BB_DATA_HTTP_MAX_DESCRIBE) is shared, first-come, no-eviction, and
        // can legitimately be full by the time this producer registers.
        // Degrade-and-continue: log a warning and fall through so the
        // queue/task/subscription below still comes up.
        bb_err_t describe_rc = bb_data_http_describe("log", "log", "LogEvent",
                                                      bb_log_event_line_get_schema());
        if (describe_rc != BB_OK) {
            bb_log_w(TAG, "log schema describe failed: %d", (int)describe_rc);
        }
    }
#else
    bb_err_t describe_rc = bb_data_http_describe("log", "log", "LogEvent",
                                                  bb_log_event_line_get_schema());
    if (describe_rc != BB_OK) {
        bb_log_w(TAG, "log schema describe failed: %d", (int)describe_rc);
    }
#endif /* CONFIG_BB_OPENAPI_RUNTIME_META */

    s_q = xQueueCreate(BB_LOG_EVENT_QUEUE_LEN, sizeof(bb_log_event_msg_t));
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
