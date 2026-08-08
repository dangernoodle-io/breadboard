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

/**
 * Synchronously drain any log output queued for async delivery, so every
 * line logged before this call is guaranteed to be on the wire (console/
 * UART) by the time it returns — the console writer normally runs on a
 * low-priority background task, so a caller that logs then resets/reboots
 * shortly after can otherwise race it and lose the line (see the boot
 * banner's own reset= field, added to make exactly this failure mode
 * diagnosable). Bounded: never blocks forever — returns BB_ERR_TIMEOUT
 * rather than hang if the writer can't keep up.
 *
 * Ordering-independent: safe to call whether or not the async writer (ESP-
 * IDF's bb_log_stream_init) has been started yet — a not-yet-started or
 * never-present writer is a synchronous no-op (bb_log lines are already on
 * the wire in that case; this just flushes libc's own stdio buffer). Also
 * safe to call from the writer task's own context (falls back to the same
 * no-op rather than deadlock waiting on itself).
 *
 * Portable across ESP-IDF/host/Arduino. Also useful generally before a
 * reboot/OTA apply so the last diagnostic line survives the reset — no
 * call site added here; left for the caller that needs it.
 *
 * Not ISR-safe — do not call from interrupt context (it may take a mutex
 * and block on a semaphore/queue).
 *
 * Arduino note: bounded on ESP-IDF/host via a hard deadline regardless of
 * how slow/stalled the sink is. On Arduino it delegates to Serial.flush(),
 * whose own contract is to block until the TX buffer drains with NO
 * timeout — an unplugged or stalled USB host can therefore hang this call
 * on that backend specifically. Documented divergence, not a silent one:
 * every other backend honors the "never blocks forever" contract above.
 */
bb_err_t bb_log_flush(void);

/**
 * Snapshot of bb_log's internal drop/loss counters -- the read surface for
 * otherwise counter-only diagnostics (console/writer queue drops, the
 * optional UDP mirror's queue drops, the event-forwarder queue drops, and
 * bb_log_flush() timeouts). Every field is copied from its own counter with
 * a single read -- per-field atomic, but the four fields together are NOT a
 * mutually consistent point-in-time snapshot: a drop can land between two
 * of the reads bb_log_drop_stats_get() performs, so don't treat this struct
 * as one atomic transaction across fields.
 *
 * udp_dropped is always 0 when CONFIG_BB_LOG_UDP_SINK is off -- the field
 * exists unconditionally so the wire shape stays config-invariant.
 */
typedef struct {
    uint32_t writer_dropped;   // console/writer queue full
    uint32_t udp_dropped;      // UDP sink queue full (always 0 when the sink is compiled out)
    uint32_t event_dropped;    // event-forwarder queue full
    uint32_t flush_timeouts;   // bb_log_flush() lock or marker-wait timeouts
} bb_log_drop_stats_t;

/**
 * Fill *out with the current drop/loss counters (see bb_log_drop_stats_t
 * above). No-op if out is NULL. Portable across ESP-IDF/host/Arduino --
 * host and Arduino builds have no async writer/UDP/event forwarder queue or
 * flush-timeout mechanism at all (bb_log_i/e/w write synchronously there),
 * so every field always reads 0 on those backends.
 */
void bb_log_drop_stats_get(bb_log_drop_stats_t *out);

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
/**
 * Fixed-width mask substituted for a secret by bb_log_secret() below.
 * Deliberately constant regardless of the real value's length: a length-
 * preserving mask (e.g. one '*' per character) leaks how long the secret
 * is, which narrows a brute-force/guessing search -- giving up nothing is
 * strictly safer than giving up the length. Exposed so tests can assert
 * against it without hardcoding "***" twice.
 */
#define BB_LOG_SECRET_MASK "***"

/**
 * The ONLY sanctioned way to put a secret-derived value (WiFi/MQTT
 * password, PSK, token, ...) into a log line. `secret`'s bytes are read
 * exactly once, to tell "unset" (NULL or "") from "set" -- they are never
 * copied into a printf-style format string or otherwise interpolated, so
 * there is no call shape that leaks the raw value through this function.
 * Emits "<label>=<mask>" (BB_LOG_SECRET_MASK) when set, "<label>=(unset)"
 * when NULL/empty, at `level`. The literal text "(unset)" can never
 * collide with a real masked secret: any non-NULL, non-empty `secret` --
 * even one whose actual value happens to BE the string "(unset)" -- always
 * takes the `secret && secret[0]` branch and is masked as
 * BB_LOG_SECRET_MASK, never emitted verbatim as the sentinel text. The
 * "(unset)" line therefore always means exactly one thing: no value was
 * supplied.
 *
 * Callers must never pre-mask a secret themselves and hand the masked
 * string to a plain bb_log_i/e/w/d/v — that bypass is exactly what let the
 * SoftAP password ship in plaintext once; route every secret through this
 * function instead, and log any non-secret context (SSID, ssid-derived
 * strings, etc.) with a normal bb_log_* call alongside it.
 *
 * `tag` and `label` are forwarded into a printf-style "%s" internally and
 * are normalized to "?" if NULL (never UB on a NULL tag/label the way a
 * bare vprintf-family "%s" would be) -- only `secret` carries the
 * documented "read once to classify unset-vs-set" NULL-safety above.
 *
 * Portable — implemented once in bb_log_secret.c and compiled on every
 * backend (ESP-IDF/Arduino/host), so the masking policy is identical
 * everywhere bb_log runs.
 */
void bb_log_secret(bb_log_level_t level, const char *tag, const char *label, const char *secret);

#ifdef ESP_PLATFORM
  #include "esp_log.h"

  /**
   * B1-1443 PR-1: the single producer for OUR OWN (bb_log_e/w/i/d/v) log
   * lines -- stamps ts at the call site (bb_clock_now_ms64()), formats the
   * message exactly once (bb_log_stream_format()), and fans out the real
   * level/tag/msg directly to every downstream consumer (console writer
   * queue, the optional bb_diag panic tap, the optional UDP mirror, and the
   * bb_log_event forwarder) -- see platform/espidf/bb_log/bb_log.c. This
   * bypasses ESP_LOGx/the vprintf hook entirely for our own calls, so the
   * bb_log_event forwarder no longer has to reconstruct level/tag/msg by
   * re-parsing already-formatted console text for lines that originated
   * here (foreign/vendored ESP_LOGx output is untouched -- it still flows
   * through the vprintf hook and is still parsed exactly as before). Not
   * called directly -- BB_LOG_X()/bb_log_e/w/i/d/v below are the call-site
   * API, and their two-layer gate (compile-time LOG_LOCAL_LEVEL, then
   * runtime esp_log_level_get(tag)) decides whether this is even invoked,
   * so a below-threshold call never pays for formatting.
   */
  void bb_log_emit(bb_log_level_t bb_level, const char *tag, const char *fmt, ...)
      __attribute__((format(printf, 3, 4)));

  /**
   * Two-layer call-site gate, shared by bb_log_e/w/i/d/v below.
   * Layer A (LOG_LOCAL_LEVEL >= esp_level) is a compile-time constant fold:
   * below LOG_LOCAL_LEVEL, the whole body -- including the fmt string
   * literal -- is dead code, so the compiler strips it exactly as it does
   * for a raw ESP_LOGx call today. Layer B (esp_log_level_get(tag) >=
   * esp_level) is the SAME per-tag registry lookup (cache + linked-list,
   * under the log lock) that ESP-IDF's own esp_log_va()/
   * esp_log_is_tag_loggable() already perform internally before touching
   * the vprintf hook -- this relocates that cost to the call site instead
   * of adding a new one.
   */
  #define BB_LOG_X(esp_level, bb_level, tag, fmt, ...)                       \
      do {                                                                   \
          if (LOG_LOCAL_LEVEL >= (esp_level)) {                              \
              if (esp_log_level_get(tag) >= (esp_level)) {                   \
                  bb_log_emit((bb_level), (tag), (fmt), ##__VA_ARGS__);      \
              }                                                              \
          }                                                                  \
      } while (0)

  #define bb_log_e(tag, fmt, ...) BB_LOG_X(ESP_LOG_ERROR,   BB_LOG_LEVEL_ERROR,   tag, fmt, ##__VA_ARGS__)
  #define bb_log_w(tag, fmt, ...) BB_LOG_X(ESP_LOG_WARN,    BB_LOG_LEVEL_WARN,    tag, fmt, ##__VA_ARGS__)
  #define bb_log_i(tag, fmt, ...) BB_LOG_X(ESP_LOG_INFO,    BB_LOG_LEVEL_INFO,    tag, fmt, ##__VA_ARGS__)
  #define bb_log_d(tag, fmt, ...) BB_LOG_X(ESP_LOG_DEBUG,   BB_LOG_LEVEL_DEBUG,   tag, fmt, ##__VA_ARGS__)
  #define bb_log_v(tag, fmt, ...) BB_LOG_X(ESP_LOG_VERBOSE, BB_LOG_LEVEL_VERBOSE, tag, fmt, ##__VA_ARGS__)
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
  #ifdef __cplusplus
  extern "C" {
  #endif
  void bb_log_emit(bb_log_level_t bb_level, const char *tag, const char *fmt, ...)
      __attribute__((format(printf, 3, 4)));
  #ifdef __cplusplus
  }
  #endif
  #define bb_log_e(tag, fmt, ...) bb_log_emit(BB_LOG_LEVEL_ERROR, (tag), (fmt), ##__VA_ARGS__)
  #define bb_log_w(tag, fmt, ...) bb_log_emit(BB_LOG_LEVEL_WARN,  (tag), (fmt), ##__VA_ARGS__)
  #define bb_log_i(tag, fmt, ...) bb_log_emit(BB_LOG_LEVEL_INFO,  (tag), (fmt), ##__VA_ARGS__)
  #define bb_log_d(tag, fmt, ...) ((void)0)
  #define bb_log_v(tag, fmt, ...) ((void)0)
#endif

#ifdef ESP_PLATFORM

/**
 * Install the async console-writer task and the custom vprintf hook.
 * Must be called once from app_main before any tasks are started.
 */
// bbtool:init tier=early fn=bb_log_stream_init
bb_err_t bb_log_stream_init(void);

/**
 * Returns true if bb_log_stream_init() has been called successfully.
 */
bool bb_log_stream_ready(void);

/**
 * Returns the count of bb_log_flush() calls (on this ESP-IDF async-writer
 * backend) that gave up with BB_ERR_TIMEOUT before their own marker was
 * confirmed drained. A nonzero count means at least one flush caller could
 * no longer be sure its preceding log lines had reached the wire before it
 * returned -- exactly the forensic gap bb_log_flush exists to close, so a
 * caller that only does `(void)bb_log_flush();` can still notice via this
 * counter. Also readable via bb_log_drop_stats_get()'s flush_timeouts field.
 */
uint32_t bb_log_flush_timeouts(void);

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
// bbtool:init tier=early fn=bb_log_config_init
bb_err_t bb_log_config_init(void);

#endif /* ESP_PLATFORM */
