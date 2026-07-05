#pragma once

// Pure "key=value,key=value" string parser — no allocation, no platform
// deps. Intended for pushing name-value pairs through sdkconfig-style
// strings (first consumer: per-tag log levels). Compiled on host, ESP-IDF,
// and Arduino.

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Invoked once per valid key=value pair found by bb_kv_parse. key/val point
/// INTO the source string passed to bb_kv_parse — they are NOT
/// NUL-terminated; use key_len/val_len. ctx is the caller-supplied pointer
/// passed through from bb_kv_parse.
typedef void (*bb_kv_cb_t)(const char *key, size_t key_len,
                            const char *val, size_t val_len, void *ctx);

/// Parse a comma-separated list of key=value pairs, invoking cb once per
/// valid pair. Never allocates and never mutates s.
///
/// - Surrounding ASCII whitespace (space/tab) around each key and value is
///   trimmed before the callback fires.
/// - An entry with no '=', or an entry whose key is empty after trimming,
///   is malformed and silently skipped (never fatal, never invokes cb).
/// - An empty value is valid: "a=" invokes cb with key="a", val_len=0.
/// - s == NULL or "" invokes no callbacks.
/// - cb == NULL is a safe no-op.
/// - Leading, trailing, and duplicate commas produce empty entries, which
///   are skipped like any other malformed entry.
void bb_kv_parse(const char *s, bb_kv_cb_t cb, void *ctx);

#ifdef __cplusplus
}
#endif
