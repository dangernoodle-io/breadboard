#include "bb_log.h"
#include "bb_mem.h"
#include <stdio.h>
#include <string.h>

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
#include "freertos/ringbuf.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "bb_task_registry.h"
#include <stdatomic.h>
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
} log_writer_msg_t;

static uint8_t *s_rb_storage = NULL;
static StaticRingbuffer_t *s_rb_static = NULL;
static RingbufHandle_t s_rb = NULL;
static vprintf_like_t s_orig_vprintf = NULL;
static bool s_ready = false;
static bool s_initialized = false;
static uint32_t s_dropped_lines = 0;

static QueueHandle_t s_writer_q = NULL;
static TaskHandle_t  s_writer_task = NULL;
static volatile uint32_t s_writer_dropped = 0;

static void s_drop_oldest(void)
{
    size_t item_size = 0;
    void *item = xRingbufferReceive(s_rb, &item_size, 0);
    if (item) {
        vRingbufferReturnItem(s_rb, item);
    }
}

/* Writer task: drains the queue and writes to stdout.
 * If USB-CDC TX blocks, only this task stalls — the log hook returns
 * immediately after enqueue, so the IDF log mutex is never held long. */
static void s_writer_task_fn(void *arg)
{
    log_writer_msg_t msg;
    for (;;) {
        if (xQueueReceive(s_writer_q, &msg, portMAX_DELAY) == pdTRUE) {
            fwrite(msg.line, 1, msg.len, stdout);
            fflush(stdout);
        }
    }
}

#if CONFIG_BB_LOG_UDP_SINK
/* Optional UDP mirror sink — a first-class output alongside the console writer
 * and ring buffer. The vprintf hook only enqueues (non-blocking); a dedicated
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
    int n = vsnprintf(msg.line, sizeof(msg.line), fmt, args);
    if (n <= 0) return 0;
    msg.len = (n < (int)sizeof(msg.line)) ? (size_t)n : sizeof(msg.line) - 1;

    /* 1. Enqueue for console writer — non-blocking, drop on full */
    if (s_writer_q && xQueueSend(s_writer_q, &msg, 0) != pdTRUE) {
        s_writer_dropped++;
    }

    /* 2. Push to ringbuf for SSE consumers */
    if (s_rb) {
        if (xRingbufferSend(s_rb, msg.line, msg.len + 1, 0) != pdTRUE) {
            for (int i = 0; i < 8; i++) {
                s_drop_oldest();
                if (xRingbufferSend(s_rb, msg.line, msg.len + 1, 0) == pdTRUE) break;
                if (i == 7) s_dropped_lines++;
            }
        }
    }

    /* 3. Notify the optional tap (e.g. bb_diag panic mirror) */
    bb_log_stream_tap_fn tap = atomic_load(&s_tap);
    if (tap) tap(msg.line, msg.len);

    /* 4. Enqueue for bb_log_event forwarder — non-blocking, drop on full.
     *    s_event_q is NULL until bb_log_event_set_queue() is called, so this
     *    step is free until the forwarder is initialized. */
    QueueHandle_t eq = s_event_q;
    if (eq && xQueueSend(eq, &msg, 0) != pdTRUE) {
        s_event_dropped++;
    }

#if CONFIG_BB_LOG_UDP_SINK
    /* 5. Enqueue for the UDP mirror — non-blocking, drop on full. sendto runs
     *    on s_udp_task, never here inside the IDF log mutex. */
    if (s_udp_enabled && s_udp_q && xQueueSend(s_udp_q, &msg, 0) != pdTRUE) {
        s_udp_dropped++;
    }
#endif

    return n;
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
        if (xTaskCreate(s_udp_task_fn, "bb_log_udp", LOG_UDP_TASK_STACK,
                        NULL, LOG_UDP_TASK_PRIO, &s_udp_task) != pdPASS) {
            bb_log_e(TAG, "udp sink task create failed");
            return;
        }
        bb_task_registry_register("bb_log_udp", LOG_UDP_TASK_STACK, s_udp_task);
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

    // Allocate ringbuffer storage, preferring PSRAM with fallback to default heap
    size_t buf_bytes = CONFIG_BB_LOG_STREAM_BUF_BYTES;
    s_rb_storage = bb_malloc_prefer_spiram(buf_bytes);
    if (!s_rb_storage) {
        bb_log_e(TAG, "ringbuffer storage allocation failed");
        return ESP_ERR_NO_MEM;
    }

    // Allocate static ringbuffer struct
    s_rb_static = bb_malloc_internal(sizeof(StaticRingbuffer_t));
    if (!s_rb_static) {
        bb_log_e(TAG, "ringbuffer struct allocation failed");
        bb_mem_free(s_rb_storage);
        s_rb_storage = NULL;
        return ESP_ERR_NO_MEM;
    }

    // Create the ringbuffer with heap-allocated storage
    s_rb = xRingbufferCreateStatic(buf_bytes, RINGBUF_TYPE_NOSPLIT,
                                    s_rb_storage, s_rb_static);
    if (!s_rb) {
        bb_log_e(TAG, "ring buffer creation failed");
        bb_mem_free(s_rb_static);
        bb_mem_free(s_rb_storage);
        s_rb_static = NULL;
        s_rb_storage = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_writer_q = xQueueCreate(LOG_WRITER_QUEUE_LEN, sizeof(log_writer_msg_t));
    if (!s_writer_q) {
        bb_log_e(TAG, "writer queue creation failed");
        vRingbufferDelete(s_rb);
        s_rb = NULL;
        bb_mem_free(s_rb_static);
        bb_mem_free(s_rb_storage);
        s_rb_static = NULL;
        s_rb_storage = NULL;
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(s_writer_task_fn, "bb_log_writer", LOG_WRITER_TASK_STACK,
                    NULL, LOG_WRITER_TASK_PRIO, &s_writer_task) != pdPASS) {
        bb_log_e(TAG, "writer task creation failed");
        vQueueDelete(s_writer_q);
        s_writer_q = NULL;
        vRingbufferDelete(s_rb);
        s_rb = NULL;
        bb_mem_free(s_rb_static);
        bb_mem_free(s_rb_storage);
        s_rb_static = NULL;
        s_rb_storage = NULL;
        return ESP_ERR_NO_MEM;
    }
    bb_task_registry_register("bb_log_writer", LOG_WRITER_TASK_STACK, s_writer_task);

    s_orig_vprintf = esp_log_set_vprintf(s_log_vprintf);
    s_initialized = true;
    s_ready = true;
    bb_log_i(TAG, "log stream initialised (%" PRIu32 " bytes)", (uint32_t)buf_bytes);
    return ESP_OK;
}

#include "bb_init.h"

#if CONFIG_BB_LOG_STREAM_AUTOREGISTER
BB_INIT_REGISTER_EARLY(bb_log_stream, bb_log_stream_init);
#endif

size_t bb_log_stream_drain(char *out_buf, size_t out_buf_len, uint32_t timeout_ms)
{
    if (!s_rb || !out_buf || out_buf_len == 0) return 0;

    // Convert timeout_ms to FreeRTOS ticks (UINT32_MAX means wait forever)
    TickType_t ticks = (timeout_ms == UINT32_MAX) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);

    size_t item_size = 0;
    char *item = xRingbufferReceive(s_rb, &item_size, ticks);
    if (!item) return 0;

    size_t copy_len = (item_size < out_buf_len) ? item_size : out_buf_len - 1;
    memcpy(out_buf, item, copy_len);
    out_buf[copy_len] = '\0';
    vRingbufferReturnItem(s_rb, item);
    return strlen(out_buf);
}

bool bb_log_stream_ready(void)
{
    return s_ready;
}

uint32_t bb_log_stream_dropped_lines(void)
{
    return s_dropped_lines;
}

#endif /* ESP_PLATFORM */
