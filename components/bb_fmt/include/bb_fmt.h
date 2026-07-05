#pragma once

// bb_fmt — portable formatting helpers (hex encode, MAC address, bool).
//
// Pure C, no ESP-IDF or platform dependencies. Compiled identically on host
// and ESP-IDF (mirrors the bb_str shape: single implementation under
// platform/host/bb_fmt/, no espidf-specific variant needed).

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// bb_fmt_hex — encode nbytes of bytes as LOWERCASE hex into dst.
//
// If sep == 0, byte-pairs are concatenated with no separator (e.g. a raw
// SHA digest or bare MAC concatenation: "deadbeef"). If sep is any other
// char, that char is inserted between each byte-pair (e.g. ':' for a MAC:
// "de:ad:be:ef").
//
// bb_strlcpy-style semantics: ALWAYS NUL-terminates dst when dstsize > 0,
// and never writes past dst[dstsize-1]. Returns the strlen of the FULL
// untruncated result — the caller detects truncation via
// (return value >= dstsize), same convention as strlcpy/bb_strlcpy.
//
//   dstsize == 0 : writes nothing to dst, returns the full untruncated
//                  length. dst may be NULL in this case only.
//   dst == NULL  : only valid when dstsize == 0.
size_t bb_fmt_hex(const uint8_t *bytes, size_t nbytes, char sep, char *dst, size_t dstsize);

// bb_fmt_mac6 — format a 6-byte MAC address as "xx:xx:xx:xx:xx:xx"
// (17 chars + NUL = 18 bytes). Convenience wrapper over
// bb_fmt_hex(mac, 6, ':', dst, dstsize).
//
// Returns false and writes NOTHING to dst if dstsize < 18. Returns true on
// success (dstsize >= 18 is sufficient).
bool bb_fmt_mac6(const uint8_t mac[6], char *dst, size_t dstsize);

// bb_fmt_bool — pure inverse of the codebase's canonical "true"/"false"
// boolean string spellings (the same spellings used throughout, e.g.
// bb_http_json_obj.c's JSON bool serialization). Returns a static string
// literal, never NULL; no buffer/heap involved, caller does not free.
const char *bb_fmt_bool(bool v);

#ifdef __cplusplus
}
#endif
