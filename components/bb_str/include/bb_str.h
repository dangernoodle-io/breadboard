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

#ifdef __cplusplus
}
#endif
