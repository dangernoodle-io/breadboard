#pragma once

// bb_str — portable string-safety helpers.
//
// Pure C, no ESP-IDF or platform dependencies. Compiled identically on host
// and ESP-IDF (mirrors the bb_ring shape: single implementation under
// platform/host/bb_str/, no espidf-specific variant needed).

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// bb_strlcpy — BSD strlcpy semantics (glibc/host libc does not provide
// strlcpy; this is a portable reimplementation, not a passthrough wrapper).
//
// Copies up to dstsize-1 bytes of src into dst, and ALWAYS NUL-terminates
// dst when dstsize > 0.
//
// Tail bytes: bytes in dst beyond the written NUL terminator are left
// UNTOUCHED (not zero-padded) — do not rely on the remainder of dst being
// zeroed.
//
// Returns strlen(src) — the length it *tried* to create. The caller detects
// truncation via (return value >= dstsize).
//
//   dstsize == 0 : writes nothing to dst, returns strlen(src). dst may be
//                  NULL in this case only.
//   dst == NULL  : only valid when dstsize == 0.
//   src == NULL  : invalid; not checked, do not pass NULL.
size_t bb_strlcpy(char *dst, const char *src, size_t dstsize);

// bb_str_field — fill a FIXED-WIDTH, length-delimited field (e.g. 802.11
// wifi_config_t ssid[32]/password[64]).
//
// Copies up to dstsize bytes of src, ZERO-PADS the remainder up to dstsize,
// and does NOT force a trailing NUL — a source that fills the field leaves
// no terminator (exactly strncpy(dst, src, dstsize) semantics).
//
// Tail bytes: bytes beyond strlen(src) (up to dstsize) are ZERO-PADDED —
// the opposite of bb_strlcpy's untouched tail.
//
// Returns strlen(src) — truncation is detectable via (return value >
// dstsize).
//
//   dstsize == 0 : writes nothing to dst.
//
// Use this for length-delimited binary fields, NOT C strings — use
// bb_strlcpy for C strings.
size_t bb_str_field(char *dst, const char *src, size_t dstsize);

// bb_str_kv_parse — pure "key=value,key=value" string splitter. No
// allocation, no platform deps. Intended for pushing name-value pairs
// through sdkconfig-style strings (first consumer: per-tag log levels).

/// Invoked once per valid key=value pair found by bb_str_kv_parse. key/val
/// point INTO the source string passed to bb_str_kv_parse — they are NOT
/// NUL-terminated; use key_len/val_len. ctx is the caller-supplied pointer
/// passed through from bb_str_kv_parse.
typedef void (*bb_str_kv_cb_t)(const char *key, size_t key_len,
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
void bb_str_kv_parse(const char *s, bb_str_kv_cb_t cb, void *ctx);

#ifdef __cplusplus
}
#endif
