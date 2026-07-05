// bb_num — portable numeric helpers.
//
// Compiled on both host (tests) and ESP-IDF. Pure C, no platform deps.

#include "bb_num.h"

int32_t bb_clampi(int32_t x, int32_t lo, int32_t hi)
{
    if (x < lo) {
        return lo;
    }
    if (x > hi) {
        return hi;
    }
    return x;
}

float bb_clampf(float x, float lo, float hi)
{
    if (x < lo) {
        return lo;
    }
    if (x > hi) {
        return hi;
    }
    return x;
}
