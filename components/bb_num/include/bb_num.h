#pragma once

// bb_num — portable numeric helpers.
//
// Pure C, no ESP-IDF or platform dependencies. Compiled identically on host
// and ESP-IDF (mirrors the bb_str shape: single implementation under
// platform/host/bb_num/, no espidf-specific variant needed).

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// bb_clampi — clamp x into the closed interval [lo, hi].
//
// Caller should pass lo <= hi. If lo > hi the result is well-defined but
// semantically meaningless: x < lo returns lo, x > hi returns hi (strict
// comparisons), so a swapped-bound call still returns a deterministic value
// — never undefined behavior — it's just not a meaningful clamp.
int32_t bb_clampi(int32_t x, int32_t lo, int32_t hi);

// bb_clampf — clamp x into the closed interval [lo, hi].
//
// Caller should pass lo <= hi. If lo > hi the result is well-defined but
// semantically meaningless: x < lo returns lo, x > hi returns hi (strict
// comparisons), so a swapped-bound call still returns a deterministic value
// — never undefined behavior — it's just not a meaningful clamp.
//
// bb_clampf(NaN, lo, hi) returns NaN unclamped: IEEE-754 comparisons
// NaN < lo and NaN > hi are both false, so neither branch fires. This is an
// explicit contract, matching fminf/fmaxf-adjacent conventions.
float bb_clampf(float x, float lo, float hi);

// bb_num_u64_to_dec — writes `v` as a decimal string into `buf` (capacity
// `cap`, including room for the NUL terminator). Hand-rolled: no snprintf,
// no locale, no libc `ll`-format-conversion dependency (portable across
// libcs that can't format 64-bit ints, e.g. newlib-nano/CONFIG_NEWLIB_NANO_
// FORMAT). `cap == 0` is a no-op (`buf` is not touched, returns 0).
// Otherwise always NUL-terminates; if the full decimal representation plus
// NUL doesn't fit `cap`, the digits are truncated (most-significant digits
// kept, least-significant dropped) and `buf` is still NUL-terminated.
//
// Returns the number of digit bytes actually written, excluding the NUL
// terminator (less than the untruncated digit count on truncation).
size_t bb_num_u64_to_dec(char *buf, size_t cap, uint64_t v);

// bb_num_i64_to_dec — signed counterpart of bb_num_u64_to_dec(). A negative
// `v` is prefixed with `-`; INT64_MIN is handled correctly (its magnitude is
// computed via an unsigned cast, never a negation that would overflow
// int64_t). Same `cap`/truncation/return-value contract as
// bb_num_u64_to_dec() -- `buf` is always NUL-terminated for any `cap > 0`,
// even when there is only room for the sign or nothing at all.
size_t bb_num_i64_to_dec(char *buf, size_t cap, int64_t v);

#ifdef __cplusplus
}
#endif
