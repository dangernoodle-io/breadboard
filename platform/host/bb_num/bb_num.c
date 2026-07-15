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

size_t bb_num_u64_to_dec(char *buf, size_t cap, uint64_t v)
{
    if (cap == 0) return 0;

    char digits[20];  // UINT64_MAX has 20 decimal digits
    int  n = 0;
    do {
        digits[n++] = (char)('0' + (v % 10));
        v /= 10;
    } while (v > 0);

    size_t avail = cap - 1;  // room for digits, excluding the NUL
    size_t written = ((size_t)n < avail) ? (size_t)n : avail;

    for (size_t i = 0; i < written; i++) buf[i] = digits[(size_t)n - 1 - i];
    buf[written] = '\0';
    return written;
}

size_t bb_num_i64_to_dec(char *buf, size_t cap, int64_t v)
{
    if (cap == 0) return 0;

    if (v >= 0) return bb_num_u64_to_dec(buf, cap, (uint64_t)v);

    if (cap == 1) {
        buf[0] = '\0';
        return 0;
    }

    buf[0] = '-';
    // Negate as unsigned so INT64_MIN (whose magnitude overflows int64_t)
    // is handled correctly.
    uint64_t mag = (uint64_t)(-(v + 1)) + 1;
    size_t n = bb_num_u64_to_dec(buf + 1, cap - 1, mag);
    return n + 1;
}
