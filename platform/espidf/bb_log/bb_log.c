#include "bb_log.h"
#include <stdio.h>

int bb_log_stream_format(char *out_buf, size_t out_buf_len, const char *fmt, va_list args)
{
    if (!out_buf || out_buf_len == 0) return -1;
    if (!fmt) {
        out_buf[0] = '\0';
        return 0;
    }
    int n = vsnprintf(out_buf, out_buf_len, fmt, args);
    if (n < 0) {
        out_buf[0] = '\0';
        return -1;
    }
    return (n < (int)out_buf_len) ? n : (int)(out_buf_len - 1);
}

#ifdef ESP_PLATFORM

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "bb_task.h"
#include "bb_clock.h"
#include <stdatomic.h>
#include <string.h>

// bb_log_flush()'s wait-loop decision core (sequence-number/stale-give
// protocol) -- pure and portable, see bb_log_internal.h. Only the FreeRTOS
// glue driving xSemaphoreTake/xQueueSend/tick arithmetic around it lives here.
#include "../../../components/bb_log/src/bb_log_internal.h"
#if CONFIG_BB_LOG_UDP_SINK
#include "lwip/sockets.h"
#include "lwip/inet.h"
#endif

static const char *TAG = "bb_log_stream";

#define LOG_STREAM_LINE_MAX      192
#define LOG_WRITER_QUEUE_LEN     CONFIG_BB_LOG_STREAM_WRITER_QUEUE_LEN
#ifdef CONFIG_BB_LOG_WRITER_TASK_STACK
#define LOG_WRITER_TASK_STACK    CONFIG_BB_LOG_WRITER_TASK_STACK
#else
#define LOG_WRITER_TASK_STACK    2048
#endif
#define LOG_WRITER_TASK_PRIO     1   /* very low; never preempts mining */

typedef struct {
    char   line[LOG_STREAM_LINE_MAX];
    size_t len;
    // Set only for a bb_log_flush() sentinel enqueued after a real caller's
    // last log line (never by s_log_vprintf) -- line/len are unused for a
    // marker. See bb_log_flush()/s_writer_task_fn for the drain protocol.
    bool     is_flush_marker;
    // Sequence number this marker was assigned (only meaningful when
    // is_flush_marker is set) -- see bb_log_flush_wait_decide.
    uint32_t seq;
} log_writer_msg_t;

// Bound every bb_log_flush() wait so it can never block forever (a wedged
// writer task -- e.g. stalled on slow UART TX -- must surface as a timeout,
// not a hang).
#define LOG_FLUSH_TIMEOUT_MS  1000

static vprintf_like_t s_orig_vprintf = NULL;
static bool s_ready = false;
static bool s_initialized = false;
static uint32_t s_dropped_lines = 0;

static QueueHandle_t s_writer_q = NULL;
static TaskHandle_t  s_writer_task = NULL;
static volatile uint32_t s_writer_dropped = 0;

// bb_log_flush() synchronization: s_flush_lock serializes concurrent flush
// callers (only one marker is ever awaited at a time -- assigning s_flush_
// next_seq and enqueueing a marker both happen while holding this lock).
// s_flush_done is given by s_writer_task_fn once per marker it drains
// (real or stale/abandoned); s_flush_last_completed_seq records WHICH
// marker that give corresponds to, written before the give so a caller that
// wakes from the take always observes the seq belonging to the give it just
// consumed. A caller whose OWN marker timed out leaves that marker in the
// queue -- the writer still drains and gives for it later, but the give is
// now "stale": bb_log_flush_wait_decide compares the completed seq against
// the caller's own target seq and RETRYs instead of falsely reporting BB_OK
// on a give that was never for its marker. Created lazily in
// bb_log_stream_init(); a creation failure there is non-fatal --
// bb_log_flush() degrades to a synchronous fflush(stdout) whenever either
// handle is NULL.
static SemaphoreHandle_t s_flush_lock = NULL;
static SemaphoreHandle_t s_flush_done = NULL;
static uint32_t s_flush_next_seq = 0;                     // only mutated under s_flush_lock
static volatile uint32_t s_flush_last_completed_seq = 0;   // written by the writer task before each give
static volatile uint32_t s_flush_timeouts = 0;             // bb_log_flush() calls that gave up before their marker completed

/* Writer task: drains the queue and writes to stdout.
 * If USB-CDC TX blocks, only this task stalls — the log hook returns
 * immediately after enqueue, so the IDF log mutex is never held long. */
static void s_writer_task_fn(void *arg)
{
    log_writer_msg_t msg;
    for (;;) {
        if (xQueueReceive(s_writer_q, &msg, portMAX_DELAY) == pdTRUE) {
            if (msg.is_flush_marker) {
                // Every entry enqueued before this marker (the queue is
                // FIFO, and s_log_vprintf runs under the IDF log mutex, so
                // enqueue order is the real log order) has already been
                // fwrite+fflush'd above by the time we get here. Record
                // which marker this is BEFORE giving the semaphore, so a
                // caller that wakes from the take always sees the seq the
                // give corresponds to (bb_log_flush_wait_decide relies on
                // this ordering to tell a genuine completion apart from a
                // stale give left over from an earlier, timed-out caller).
                s_flush_last_completed_seq = msg.seq;
                fflush(stdout);
                if (s_flush_done) xSemaphoreGive(s_flush_done);
                continue;
            }
            fwrite(msg.line, 1, msg.len, stdout);
            fflush(stdout);
        }
    }
}

#if CONFIG_BB_LOG_UDP_SINK
/* Optional UDP mirror sink — a first-class output alongside the console writer.
 * The vprintf hook only enqueues (non-blocking); a dedicated
 * low-priority task owns the socket and does the sendto, so the blocking call
 * never runs inside the ESP-IDF log mutex. Default-compiled-out (Kconfig n). */
#define LOG_UDP_QUEUE_LEN     16
#define LOG_UDP_TASK_STACK    3072
#define LOG_UDP_TASK_PRIO     1

static QueueHandle_t       s_udp_q = NULL;
static TaskHandle_t        s_udp_task = NULL;
static volatile bool       s_udp_enabled = false;
static volatile uint32_t   s_udp_ip_be = 0;     /* network-order IPv4 */
static volatile uint16_t   s_udp_port = 0;
static volatile uint32_t   s_udp_dropped = 0;

static void s_udp_task_fn(void *arg)
{
    (void)arg;
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd >= 0) {
        int on = 1;
        setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));
    }
    log_writer_msg_t msg;
    for (;;) {
        if (xQueueReceive(s_udp_q, &msg, portMAX_DELAY) != pdTRUE) continue;
        if (fd < 0 || !s_udp_enabled) continue;
        struct sockaddr_in dst = { 0 };
        dst.sin_family      = AF_INET;
        dst.sin_port        = htons(s_udp_port);
        dst.sin_addr.s_addr = s_udp_ip_be;
        sendto(fd, msg.line, msg.len, 0, (struct sockaddr *)&dst, sizeof(dst));
    }
}
#endif /* CONFIG_BB_LOG_UDP_SINK */

// Optional tap installed by bb_diag (or any consumer) to observe every line
static _Atomic(bb_log_stream_tap_fn) s_tap;

void bb_log_stream_set_tap(bb_log_stream_tap_fn fn)
{
    atomic_store(&s_tap, fn);
}

// Event-forwarder queue — set by bb_log_event.c via bb_log_event_set_queue().
// NULL until bb_log_event initializes; the s_log_vprintf step is a no-op until then.
static volatile QueueHandle_t s_event_q = NULL;
static volatile uint32_t s_event_dropped = 0;

void bb_log_event_set_queue(QueueHandle_t q)
{
    s_event_q = q;
}

uint32_t bb_log_event_dropped(void)
{
    return s_event_dropped;
}

static int s_log_vprintf(const char *fmt, va_list args)
{
    /* Format once into a stack-allocated message */
    log_writer_msg_t msg;
    msg.is_flush_marker = false;
    int n = vsnprintf(msg.line, sizeof(msg.line), fmt, args);
    if (n <= 0) return 0;
    msg.len = (n < (int)sizeof(msg.line)) ? (size_t)n : sizeof(msg.line) - 1;

    /* 1. Enqueue for console writer — non-blocking, drop on full. Console
     *    output is unaffected by TELEM routing: every line always reaches
     *    it, regardless of the check below. */
    if (s_writer_q && xQueueSend(s_writer_q, &msg, 0) != pdTRUE) {
        s_writer_dropped++;
    }

    /* B1-1443 PR-2: this call site only ever sees FOREIGN/vendored ESP_LOGx
     * output now -- our own bb_log_e/w/i/d/v calls bypass s_log_vprintf
     * entirely via bb_log_emit() (B1-1443 PR-1). Foreign lines are wrapped
     * opaque downstream (no parse, no extracted tag -- see bb_log_event.c's
     * forwarder), so the TELEM console-only gate (bb_log_telem_route_wide(),
     * bb_log_internal.h) can never apply to them: a tag-less line always
     * routes wide, regardless of CONFIG_BB_LOG_TELEM_ROUTE_EVENTS. The TELEM
     * gate still applies to OUR OWN lines, via bb_log_emit()'s own direct
     * bb_log_telem_route_wide() call below. The console writer (step 1) and
     * the bb_diag tap (step 2) are NEVER gated -- every line always reaches
     * them regardless of route_wide.
     *
     * `route_wide` is DELIBERATELY kept as a variable here, not inlined as
     * a literal `true` at each of its two guard sites below (steps 3/4) --
     * it is a compile-time constant today (foreign lines always route wide
     * by design, as above), but keeping the same name/shape as
     * bb_log_emit()'s real conditional `route_wide` (which genuinely varies
     * per call) is purely for symmetry between the two producers'
     * near-identical fan-out code. This is vestigial by design, not a bug or
     * dead code left behind by accident. */
    bool route_wide = true;

    /* 2. Notify the optional tap (e.g. bb_diag panic mirror) — unaffected by
     *    TELEM routing, same as the console writer. */
    bb_log_stream_tap_fn tap = atomic_load(&s_tap);
    if (tap) tap(msg.line, msg.len);

    /* 3. Enqueue for bb_log_event forwarder — non-blocking, drop on full.
     *    s_event_q is NULL until bb_log_event_set_queue() is called, so this
     *    step is free until the forwarder is initialized. B1-1443 PR-2: this
     *    line always originates from foreign/vendored ESP_LOGx output now --
     *    wrap the already-formatted console text as structured=false; the
     *    forwarder wraps it opaque (no parse), see bb_log_event.c. */
    QueueHandle_t eq = s_event_q;
    if (route_wide && eq) {
        bb_log_event_msg_t emsg = { .structured = false };
        emsg.len = (msg.len < sizeof(emsg.line)) ? msg.len : sizeof(emsg.line) - 1;
        memcpy(emsg.line, msg.line, emsg.len);
        if (xQueueSend(eq, &emsg, 0) != pdTRUE) {
            s_event_dropped++;
        }
    }

#if CONFIG_BB_LOG_UDP_SINK
    /* 4. Enqueue for the UDP mirror — non-blocking, drop on full. sendto runs
     *    on s_udp_task, never here inside the IDF log mutex. Gated by
     *    route_wide same as step 3: a TELEM line kept console-only must
     *    not leak over the network either. */
    if (route_wide && s_udp_enabled && s_udp_q && xQueueSend(s_udp_q, &msg, 0) != pdTRUE) {
        s_udp_dropped++;
    }
#endif

    return n;
}

// Message-text-only cap for bb_log_emit()'s single bb_log_stream_format()
// call -- mirrors bb_log_event's own forwarder msgbuf[168] ("160 + some
// margin for safe_copy", platform/espidf/bb_log_event/bb_log_event.c). This
// is deliberately smaller than LOG_STREAM_LINE_MAX (192): the console line
// built from it still has to fit decoration (level/ts/tag) in that same
// 192-byte buffer, so a generous-but-bounded message cap here leaves room.
#define LOG_EMIT_MSG_MAX 168

/**
 * B1-1443 PR-1 single producer for OUR OWN (bb_log_e/w/i/d/v) log lines --
 * see the doc comment on the declaration in bb_log.h for the full design.
 * Never called directly from below LOG_LOCAL_LEVEL / the per-tag runtime
 * gate (see BB_LOG_X in bb_log.h) -- by the time this runs, both checks
 * have already passed.
 */
void bb_log_emit(bb_log_level_t bb_level, const char *tag, const char *fmt, ...)
{
    if (!tag) tag = "?";

    // Stamp ts at the call site, not later in the forwarder task -- fixes
    // the fidelity gap the old bb_log_event.c forwarder had (it stamped ts
    // when it happened to drain the queue, not when the line was logged).
    uint64_t ts_ms = bb_clock_now_ms64();
    char level_ch = bb_log_level_console_letter(bb_level);

    // Format the message exactly once; every consumer below reuses this
    // buffer instead of re-formatting or re-parsing.
    char msgbuf[LOG_EMIT_MSG_MAX];
    va_list args;
    va_start(args, fmt);
    int n = bb_log_stream_format(msgbuf, sizeof(msgbuf), fmt, args);
    va_end(args);
    size_t msg_len = (n > 0) ? (size_t)n : 0;

    // ---- 1. Console writer queue -- build a console-shaped line
    //         ("L (ts) tag: msg\n"), the same shape ESP_LOGx's LOG_FORMAT()
    //         has always produced, so console output is unaffected. Every
    //         line always reaches this queue, regardless of TELEM routing.
    log_writer_msg_t line_msg;
    line_msg.is_flush_marker = false;
    line_msg.len = bb_log_emit_build_line(line_msg.line, sizeof(line_msg.line),
                                           level_ch, ts_ms, tag, msgbuf, msg_len);

    if (s_writer_q) {
        if (xQueueSend(s_writer_q, &line_msg, 0) != pdTRUE) {
            s_writer_dropped++;
        }
    } else {
        // Stream not yet initialized (early boot, before bb_log_stream_init
        // runs) or its writer task failed to come up: print synchronously
        // rather than silently dropping the line. Mirrors what a bb_log_*
        // call issued this early got for free before this PR -- ESP-IDF's
        // own default (pre-hook) vprintf, which writes straight to stdout.
        // fflush() immediately, matching every other stdout write site in
        // this file (s_writer_task_fn, s_log_vprintf's default backend,
        // bb_log_flush) -- this fallback exists specifically so an early
        // line isn't lost, which a missing flush would otherwise defeat if
        // a crash follows shortly after.
        fwrite(line_msg.line, 1, line_msg.len, stdout);
        fflush(stdout);
    }

    // ---- 2. bb_diag panic tap -- unaffected by TELEM routing, same as the
    //         console writer (mirrors s_log_vprintf's step 2). Called
    //         unconditionally, gated only by the tap itself being NULL: the
    //         only setter is bb_diag_panic_init() (bb_log_stream_set_tap(),
    //         see bb_log.h), and that function calls bb_log_stream_init()
    //         itself, idempotently, immediately before installing the tap
    //         (platform/espidf/bb_diag/bb_diag_panic.c) -- so a non-NULL tap
    //         here can never be observed before the writer-queue state step
    //         1 above depends on has already settled, regardless of which
    //         order a composition root's generated init list happens to run
    //         bb_log_stream_init() and bb_diag_panic_init() in.
    bb_log_stream_tap_fn tap = atomic_load(&s_tap);
    if (tap) tap(line_msg.line, line_msg.len);

    // TELEM wide-routing decision for OUR OWN lines -- the tag is already
    // known here (the call site's own argument), so no parse is ever
    // needed. s_log_vprintf's foreign-line path can no longer make this
    // same decision (B1-1443 PR-2 removed its tag source); it always
    // routes wide instead.
#if CONFIG_BB_LOG_TELEM_ROUTING
    bool route_wide = bb_log_telem_route_wide(bb_log_telem_route_get(), tag);
#else
    bool route_wide = true;
#endif

    // ---- 3. bb_log_event forwarder -- structured, no parse: the real
    //         level/tag/msg/ts this call site already has, handed straight
    //         to the same queue s_log_vprintf's step 3 feeds.
    QueueHandle_t eq = s_event_q;
    if (route_wide && eq) {
        bb_log_event_msg_t emsg;
        bb_log_emit_build_event_msg(&emsg, level_ch, tag, ts_ms, msgbuf, msg_len);
        if (xQueueSend(eq, &emsg, 0) != pdTRUE) {
            s_event_dropped++;
        }
    }

#if CONFIG_BB_LOG_UDP_SINK
    // ---- 4. UDP mirror -- same decorated console line as the writer
    //         queue (step 1), gated by route_wide same as step 3.
    if (route_wide && s_udp_enabled && s_udp_q && xQueueSend(s_udp_q, &line_msg, 0) != pdTRUE) {
        s_udp_dropped++;
    }
#endif
}

#if CONFIG_BB_LOG_UDP_SINK
void bb_log_udp_enable(uint32_t ip_be, uint16_t port)
{
    s_udp_ip_be = ip_be;
    s_udp_port  = port;
    if (!s_udp_q) {
        s_udp_q = xQueueCreate(LOG_UDP_QUEUE_LEN, sizeof(log_writer_msg_t));
        if (!s_udp_q) {
            bb_log_e(TAG, "udp sink queue alloc failed");
            return;
        }
    }
    if (!s_udp_task) {
        bb_task_config_t udp_cfg = {
            .entry       = s_udp_task_fn,
            .name        = "bb_log_udp",
            .arg         = NULL,
            .stack_bytes = LOG_UDP_TASK_STACK,
            .priority    = LOG_UDP_TASK_PRIO,
            .core        = BB_TASK_CORE_ANY,
            .backing     = BB_TASK_BACKING_DYNAMIC,
            .wdt_arm     = false,
        };
        if (bb_task_create(&udp_cfg, (void **)&s_udp_task) != BB_OK) {
            bb_log_e(TAG, "udp sink task create failed");
            return;
        }
    }
    s_udp_enabled = true;
    bb_log_i(TAG, "udp log sink enabled -> port %u", (unsigned)port);
}

void bb_log_udp_disable(void)
{
    s_udp_enabled = false;
}
#endif /* CONFIG_BB_LOG_UDP_SINK */

bb_err_t bb_log_stream_init(void)
{
    // Idempotent: if already initialized, return success
    if (s_initialized) {
        return ESP_OK;
    }

    s_writer_q = xQueueCreate(LOG_WRITER_QUEUE_LEN, sizeof(log_writer_msg_t));
    if (!s_writer_q) {
        bb_log_e(TAG, "writer queue creation failed");
        return ESP_ERR_NO_MEM;
    }

    bb_task_config_t writer_cfg = {
        .entry       = s_writer_task_fn,
        .name        = "bb_log_writer",
        .arg         = NULL,
        .stack_bytes = LOG_WRITER_TASK_STACK,
        .priority    = LOG_WRITER_TASK_PRIO,
        .core        = BB_TASK_CORE_ANY,
        .backing     = BB_TASK_BACKING_DYNAMIC,
        .wdt_arm     = false,
    };
    if (bb_task_create(&writer_cfg, (void **)&s_writer_task) != BB_OK) {
        bb_log_e(TAG, "writer task creation failed");
        vQueueDelete(s_writer_q);
        s_writer_q = NULL;
        return ESP_ERR_NO_MEM;
    }

    // bb_log_flush() sync primitives -- non-fatal if either allocation
    // fails: bb_log_flush() checks both handles and falls back to a
    // synchronous fflush(stdout) whenever one is NULL, so a degraded flush
    // is safe, just not a genuine hard guarantee that the writer has caught
    // up. Created after the writer task so a caller can never observe the
    // task running without them.
    s_flush_lock = xSemaphoreCreateMutex();
    s_flush_done = xSemaphoreCreateBinary();
    if (!s_flush_lock || !s_flush_done) {
        bb_log_w(TAG, "flush sync primitives creation failed -- bb_log_flush degrades to fflush(stdout)");
    }

    s_orig_vprintf = esp_log_set_vprintf(s_log_vprintf);
    s_initialized = true;
    s_ready = true;
    bb_log_i(TAG, "log stream initialised");
    return ESP_OK;
}

bb_err_t bb_log_flush(void)
{
    // No async writer (bb_log_stream_init never called, or its flush
    // primitives failed to allocate): ESP_LOG's default vprintf writes
    // synchronously, so nothing is queued -- just push whatever libc has
    // buffered.
    if (!s_writer_q || !s_writer_task || !s_flush_lock || !s_flush_done) {
        fflush(stdout);
        return BB_OK;
    }

    // Called from the writer task's own context: it can't service its own
    // marker while parked below waiting for it (deadlock), and it already
    // fflush()es after every line it drains, so there is nothing left to
    // wait for.
    if (xTaskGetCurrentTaskHandle() == s_writer_task) {
        fflush(stdout);
        return BB_OK;
    }

    // Serialize concurrent callers -- s_flush_done is a single shared
    // semaphore; two overlapping flushes racing on it could let one caller
    // consume the signal meant for the other. Also protects s_flush_next_seq
    // (only ever mutated here, one caller at a time).
    if (xSemaphoreTake(s_flush_lock, pdMS_TO_TICKS(LOG_FLUSH_TIMEOUT_MS)) != pdTRUE) {
        s_flush_timeouts++;
        return BB_ERR_TIMEOUT;
    }

    uint32_t my_seq = ++s_flush_next_seq;
    log_writer_msg_t marker = { .is_flush_marker = true, .seq = my_seq };
    bb_err_t rc;

    if (xQueueSend(s_writer_q, &marker, pdMS_TO_TICKS(LOG_FLUSH_TIMEOUT_MS)) != pdTRUE) {
        // Never enqueued -- no stale marker left behind to worry about.
        rc = BB_ERR_TIMEOUT;
    } else {
        // Wait for OUR marker specifically, not just any give: a give can
        // arrive for an earlier caller's marker that timed out here on a
        // previous call (still sitting in the queue, since a timed-out
        // caller can't un-enqueue it) -- bb_log_flush_wait_decide tells that
        // stale give apart from a genuine completion via the sequence
        // number the writer records before each give (s_flush_last_
        // completed_seq). The whole retry loop shares ONE overall deadline
        // (LOG_FLUSH_TIMEOUT_MS from here) rather than restarting the
        // timeout on every stale give -- see bb_log_flush_remaining_ms.
        TickType_t start_tick = xTaskGetTickCount();
        rc = BB_ERR_TIMEOUT;  // overwritten below unless the loop times out
        for (;;) {
            uint32_t elapsed_ms = (uint32_t)((xTaskGetTickCount() - start_tick) * portTICK_PERIOD_MS);
            uint32_t remaining_ms = bb_log_flush_remaining_ms(LOG_FLUSH_TIMEOUT_MS, elapsed_ms);
            if (remaining_ms == 0) {
                break;  // rc stays BB_ERR_TIMEOUT
            }

            // Floor the block time at 1 tick: at a coarse tick rate (e.g.
            // 100 Hz), pdMS_TO_TICKS(1..9) rounds down to 0, which makes
            // xSemaphoreTake non-blocking -- a RETRY landing in that tail of
            // the budget would otherwise hot-spin on xTaskGetTickCount()
            // above the writer task's priority (the very task the wait is
            // waiting on) until the deadline. The floor only changes how
            // long THIS take blocks, never the overall deadline -- the
            // remaining_ms==0 check above still owns exiting the loop once
            // the real budget is spent, so a floored take can, at worst,
            // overrun by under one tick before the next iteration reads
            // remaining_ms==0 and exits.
            TickType_t block_ticks = pdMS_TO_TICKS(remaining_ms);
            if (block_ticks == 0) block_ticks = 1;
            BaseType_t took = xSemaphoreTake(s_flush_done, block_ticks);
            bb_log_flush_wait_step_t step =
                bb_log_flush_wait_decide(took == pdTRUE, s_flush_last_completed_seq, my_seq);
            if (step == BB_LOG_FLUSH_WAIT_DONE) {
                rc = BB_OK;
                break;
            }
            if (step == BB_LOG_FLUSH_WAIT_TIMEOUT) {
                break;  // rc stays BB_ERR_TIMEOUT
            }
            // BB_LOG_FLUSH_WAIT_RETRY: that give belonged to an earlier,
            // abandoned marker -- loop and take again on the remaining budget.
        }
    }

    if (rc != BB_OK) {
        s_flush_timeouts++;
    }

    xSemaphoreGive(s_flush_lock);
    return rc;
}

/**
 * Count of bb_log_flush() calls that gave up (BB_ERR_TIMEOUT) before their
 * own marker was confirmed drained -- mirrors bb_log_stream_dropped_lines().
 * A nonzero count means at least one flush caller can no longer be sure its
 * preceding log lines reached the wire before it returned (the exact
 * forensic gap bb_log_flush exists to close) -- surfaced here rather than
 * only discarded at call sites like the boot banner's.
 */
uint32_t bb_log_flush_timeouts(void)
{
    return s_flush_timeouts;
}

bool bb_log_stream_ready(void)
{
    return s_ready;
}

uint32_t bb_log_stream_dropped_lines(void)
{
    return s_dropped_lines;
}

#elif !defined(ARDUINO)
// Host build: bb_log_i/e/w (bb_log.h's non-ESP_PLATFORM branch) write
// straight through fprintf, so nothing is ever queued -- flushing just
// needs to push whatever libc has buffered. (Arduino provides its own
// definition in platform/arduino/bb_log/bb_log_arduino.cpp -- excluded here
// in case this file is ever compiled alongside it.)
bb_err_t bb_log_flush(void)
{
    fflush(stdout);
    return BB_OK;
}
#endif /* ESP_PLATFORM */
