#pragma once

// bb_num — portable numeric helpers.
//
// Pure C, no ESP-IDF or platform dependencies. Compiled identically on host
// and ESP-IDF (mirrors the bb_str shape: single implementation under
// platform/host/bb_num/, no espidf-specific variant needed).

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

#ifdef __cplusplus
}
#endif
