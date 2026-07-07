#pragma once

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "bb_core.h"

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

// Single source of truth for bb_log_level_t <-> string mappings. X(level, name)
// is invoked once per level, in wire-format order. Drives bb_log_level_to_str,
// bb_log_level_from_str, and the GET /api/log/level level-name list — keeping
// all three in lockstep instead of three independently hand-maintained tables.
#define BB_LOG_LEVEL_LIST(X)          \
    X(BB_LOG_LEVEL_NONE,    "none")    \
    X(BB_LOG_LEVEL_ERROR,   "error")   \
    X(BB_LOG_LEVEL_WARN,    "warn")    \
    X(BB_LOG_LEVEL_INFO,    "info")    \
    X(BB_LOG_LEVEL_DEBUG,   "debug")   \
    X(BB_LOG_LEVEL_VERBOSE, "verbose")

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

/**
 * Tap callback signature: receives every formatted log line written through
 * the stream. Called from inside the vprintf hook — implementations MUST be
 * lock-free, allocation-free, and bounded in time. `data` is NOT NUL-
 * terminated; respect `len`.
 */
typedef void (*bb_log_stream_tap_fn)(const char *data, size_t len);

/**
 * Install a tap that observes every line written to the log stream.
 * Pass NULL to remove. At most one tap is active at a time; calling twice
 * replaces the previous tap. Used by bb_diag to mirror lines into the RTC
 * panic buffer without coupling bb_log to bb_diag.
 */
void bb_log_stream_set_tap(bb_log_stream_tap_fn fn);

#if CONFIG_BB_LOG_UDP_SINK
/**
 * Mirror every formatted log line to a UDP datagram target (one datagram per
 * line) — a first-class sink alongside the console writer and SSE ring. The
 * log hook only enqueues; a dedicated low-priority task owns the socket and
 * does the sendto, so the blocking call never runs inside the IDF log mutex.
 * Intended for headless / pre-httpd states (e.g. OTA-only boot mode) where the
 * SSE HTTP stream isn't available. `ip_be` is a network-order IPv4 address
 * (INADDR_BROADCAST = 255.255.255.255 for subnet broadcast). Lazily spawns the
 * sender task on first enable. Compiled out unless CONFIG_BB_LOG_UDP_SINK=y.
 * NOTE: log lines may carry SSID/hostname — enable deliberately.
 */
void bb_log_udp_enable(uint32_t ip_be, uint16_t port);

/** Stop mirroring to UDP (task/socket retained; enqueue gated off). */
void bb_log_udp_disable(void);
#endif /* CONFIG_BB_LOG_UDP_SINK */

/**
 * Apply CONFIG_BB_LOG_DEFAULT_LEVEL / CONFIG_BB_LOG_LEVELS to the log-level
 * registry. Hand-wire entry point (components/bb_log/src/bb_log_config.c) --
 * bb_log has no self-registration; the consumer's app_main must call this
 * directly.
 */
bb_err_t bb_log_config_init(void);

#endif /* ESP_PLATFORM */
