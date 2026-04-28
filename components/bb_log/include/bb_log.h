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

typedef enum {
    BB_LOG_LEVEL_NONE,
    BB_LOG_LEVEL_ERROR,
    BB_LOG_LEVEL_WARN,
    BB_LOG_LEVEL_INFO,
    BB_LOG_LEVEL_DEBUG,
    BB_LOG_LEVEL_VERBOSE,
} bb_log_level_t;

// Set log level for a tag. Use tag="*" to set the global default.
// Registers the tag in the internal registry; on first registration applies default_level.
// ESP-IDF: also calls esp_log_level_set. Arduino/host: no-op for backend, registry still works.
void bb_log_level_set(const char *tag, bb_log_level_t level);

// Parse "error"/"warn"/"info"/"debug"/"verbose"/"none" (case-insensitive).
// Returns false on unknown string. Portable — used by the HTTP handler.
bool bb_log_level_from_str(const char *s, bb_log_level_t *out);

// Return string representation of a log level: "none", "error", ..., "verbose".
const char *bb_log_level_to_str(bb_log_level_t level);

// Register a tag as configurable at runtime. Copies `tag` into internal storage.
// On first registration, also applies `level` via bb_log_level_set.
// Idempotent: calling twice with the same tag is a no-op (does not re-apply level).
// Silently dropped if internal registry is full.
void bb_log_tag_register(const char *tag, bb_log_level_t level);

// Read currently-tracked level for a tag. Returns true if tag is in the registry.
bool bb_log_tag_level(const char *tag, bb_log_level_t *out);

// Iterate registry entries. Pass index starting at 0; returns false when exhausted.
// Output pointers remain valid until next bb_log_tag_register / bb_log_level_set.
bool bb_log_tag_at(size_t index, const char **tag_out, bb_log_level_t *level_out);

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
#elif defined(ARDUINO)
  #include <Arduino.h>
  #ifdef __cplusplus
  extern "C" {
  #endif
  void bb_log_arduino_emit(char level, const char *tag, const char *fmt, ...);
  #ifdef __cplusplus
  }
  #endif
  #define bb_log_e(tag, fmt, ...) bb_log_arduino_emit('E', (tag), (fmt), ##__VA_ARGS__)
  #define bb_log_w(tag, fmt, ...) bb_log_arduino_emit('W', (tag), (fmt), ##__VA_ARGS__)
  #define bb_log_i(tag, fmt, ...) bb_log_arduino_emit('I', (tag), (fmt), ##__VA_ARGS__)
  #define bb_log_d(tag, fmt, ...) ((void)0)
  #define bb_log_v(tag, fmt, ...) ((void)0)
#else
  #include <stdio.h>
  #define bb_log_e(tag, fmt, ...) fprintf(stderr, "E (%s) " fmt "\n", (tag), ##__VA_ARGS__)
  #define bb_log_w(tag, fmt, ...) fprintf(stderr, "W (%s) " fmt "\n", (tag), ##__VA_ARGS__)
  #define bb_log_i(tag, fmt, ...) fprintf(stdout, "I (%s) " fmt "\n", (tag), ##__VA_ARGS__)
  #define bb_log_d(tag, fmt, ...) ((void)0)
  #define bb_log_v(tag, fmt, ...) ((void)0)
#endif

#ifdef ESP_PLATFORM

#include "bb_nv.h"
#include "freertos/FreeRTOS.h"

/**
 * Initialise the ring buffer and install the custom vprintf hook.
 * Must be called once from app_main before any tasks are started.
 */
bb_err_t bb_log_stream_init(void);

/**
 * Drain queued log bytes into out_buf (up to out_buf_len).
 * Waits up to timeout_ms for data if the ring is empty; UINT32_MAX means block forever.
 * Returns the number of bytes written (excluding NUL), or 0 on timeout.
 */
size_t bb_log_stream_drain(char *out_buf, size_t out_buf_len, uint32_t timeout_ms);

/**
 * Returns true if bb_log_stream_init() has been called successfully.
 */
bool bb_log_stream_ready(void);

/**
 * Returns the count of lines that could not be sent despite the drop-oldest loop.
 */
uint32_t bb_log_stream_dropped_lines(void);

#endif /* ESP_PLATFORM */
