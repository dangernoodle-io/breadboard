// bb_led_gamma.h — perceptual brightness correction for bb_led backends.
// Semi-private (like bb_led_driver.h): included by driver .c files, not consumers.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Map a perceptual brightness `level` (0..65535, linear in human lightness) to a
// linear-luminance output (0..65535) via the CIE 1931 L* curve. Backends scale
// the result to their native depth (PWM duty bits, 8-bit RGB, etc.).
//
// CIE L* (not gamma 2.2) is used because it matches the eye's response at the
// dim end, where a naive linear duty ramp is visibly steppy. Implemented as a
// 257-entry uint16_t LUT with linear interpolation — no <math.h>, ~514 B flash,
// one multiply + one add per call. bb_led_gamma_cie(0)==0, (65535)==65535.
uint32_t bb_led_gamma_cie(uint16_t level);

#ifdef __cplusplus
}
#endif
