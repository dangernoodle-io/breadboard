#include "bb_log.h"
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

#include "freertos/ringbuf.h"

static const char *TAG = "bb_log_stream";

#define LOG_STREAM_BUF_BYTES     6144
#define LOG_STREAM_LINE_MAX      192
#define LOG_WRITER_QUEUE_LEN     32
#define LOG_WRITER_TASK_STACK    2048
#define LOG_WRITER_TASK_PRIO     1   /* very low; never preempts mining */

typedef struct {
    char   line[LOG_STREAM_LINE_MAX];
    size_t len;
} log_writer_msg_t;

static uint8_t s_rb_storage[LOG_STREAM_BUF_BYTES];
static StaticRingbuffer_t s_rb_static;
static RingbufHandle_t s_rb = NULL;
static vprintf_like_t s_orig_vprintf = NULL;
static bool s_ready = false;
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

    return n;
}

bb_err_t bb_log_stream_init(void)
{
    s_rb = xRingbufferCreateStatic(LOG_STREAM_BUF_BYTES, RINGBUF_TYPE_NOSPLIT,
                                    s_rb_storage, &s_rb_static);
    if (!s_rb) {
        bb_log_e(TAG, "ring buffer creation failed");
        return ESP_ERR_NO_MEM;
    }

    s_writer_q = xQueueCreate(LOG_WRITER_QUEUE_LEN, sizeof(log_writer_msg_t));
    if (!s_writer_q) {
        bb_log_e(TAG, "writer queue creation failed");
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(s_writer_task_fn, "bb_log_writer", LOG_WRITER_TASK_STACK,
                    NULL, LOG_WRITER_TASK_PRIO, &s_writer_task) != pdPASS) {
        bb_log_e(TAG, "writer task creation failed");
        vQueueDelete(s_writer_q);
        s_writer_q = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_orig_vprintf = esp_log_set_vprintf(s_log_vprintf);
    s_ready = true;
    bb_log_i(TAG, "log stream initialised (%" PRIu32 " bytes)", (uint32_t)LOG_STREAM_BUF_BYTES);
    return ESP_OK;
}

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
