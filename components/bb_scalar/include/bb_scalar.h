#pragma once

// bb_scalar — portable scalar parsing primitives.
//
// Pure C, no ESP-IDF or platform dependencies. Compiled identically on host
// and ESP-IDF (mirrors the bb_str shape: single implementation under
// platform/host/bb_scalar/, no espidf-specific variant needed).
//
// Behavior-equivalent to bb_http_server's former bb_url_parse_bool/
// bb_url_parse_uint (components/bb_http_server/src/http_utils.c) — extracted
// here as host-portable primitives so non-HTTP callers don't need to depend
// on bb_http_server. Migration complete: the originals have been removed
// from bb_http_server, and the scalar_parse fence is drained to zero and
// locked — reintroducing either symbol (or a hand-rolled duplicate) outside
// this component is blocked.

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// bb_scalar_parse_bool — strict boolean parser for form-urlencoded-style
// values. Returns true on success (with the parsed boolean in *out); false
// on NULL/empty/garbage input (in which case *out is left unwritten).
// Case-insensitive: 1/true/on/yes/t/y -> true; 0/false/off/no/f/n -> false.
// Anything else (e.g. "maybe", "2", "") returns false without writing *out.
bool bb_scalar_parse_bool(const char *val, bool *out);

// bb_scalar_parse_uint — strict unsigned integer parser. Returns true on
// success (with the parsed value in *out); false on NULL/empty/garbage/
// overflow input (in which case *out is left unwritten). Accepts only a
// run of decimal digits — no leading sign, no whitespace, no trailing
// junk. Overflow (value exceeding ULONG_MAX) is rejected via errno/ERANGE
// from the underlying strtoul call.
bool bb_scalar_parse_uint(const char *val, unsigned long *out);

#ifdef __cplusplus
}
#endif
