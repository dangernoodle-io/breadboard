#pragma once

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * Format fmt+args into out_buf (like vsnprintf).
 * Platform-independent — exposed for host tests.
 * Returns number of bytes written (excluding NUL), or negative on error.
 */
int bb_log_stream_format(char *out_buf, size_t out_buf_len, const char *fmt, va_list args);

/*
 * bb_log_{e,w,i,d,v}(tag, fmt, ...) — platform-abstract logging macros.
 * On ESP-IDF, expand to ESP_LOG{E,W,I,D,V}. On host, expand to
 * fprintf(stderr|stdout, ...). Debug and verbose are compiled out on host
 * to keep test output clean.
 */
#ifdef ESP_PLATFORM
  #include "esp_log.h"
  #define bb_log_e(tag, fmt, ...) ESP_LOGE(tag, fmt, ##__VA_ARGS__)
  #define bb_log_w(tag, fmt, ...) ESP_LOGW(tag, fmt, ##__VA_ARGS__)
  #define bb_log_i(tag, fmt, ...) ESP_LOGI(tag, fmt, ##__VA_ARGS__)
  #define bb_log_d(tag, fmt, ...) ESP_LOGD(tag, fmt, ##__VA_ARGS__)
  #define bb_log_v(tag, fmt, ...) ESP_LOGV(tag, fmt, ##__VA_ARGS__)
#else
  #include <stdio.h>
  #define bb_log_e(tag, fmt, ...) fprintf(stderr, "E (%s) " fmt "\n", (tag), ##__VA_ARGS__)
  #define bb_log_w(tag, fmt, ...) fprintf(stderr, "W (%s) " fmt "\n", (tag), ##__VA_ARGS__)
  #define bb_log_i(tag, fmt, ...) fprintf(stdout, "I (%s) " fmt "\n", (tag), ##__VA_ARGS__)
  #define bb_log_d(tag, fmt, ...) ((void)0)
  #define bb_log_v(tag, fmt, ...) ((void)0)
#endif

#ifdef ESP_PLATFORM

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

/**
 * Initialise the ring buffer and install the custom vprintf hook.
 * Must be called once from app_main before any tasks are started.
 */
esp_err_t bb_log_stream_init(void);

/**
 * Drain one log line from the ring buffer into out_buf.
 * Blocks for up to ticks_to_wait FreeRTOS ticks.
 * Returns the number of bytes written (excluding NUL), or 0 on timeout.
 */
size_t bb_log_stream_drain(char *out_buf, size_t out_buf_len, uint32_t ticks_to_wait);

/**
 * Returns true if bb_log_stream_init() has been called successfully.
 */
bool bb_log_stream_ready(void);

/**
 * Returns the count of lines that could not be sent despite the drop-oldest loop.
 */
uint32_t bb_log_stream_dropped_lines(void);

#endif /* ESP_PLATFORM */
