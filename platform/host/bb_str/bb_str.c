// bb_str — portable string-safety helpers.
//
// Compiled on both host (tests) and ESP-IDF. Pure C, no platform deps.

#include "bb_str.h"

size_t bb_strlcpy(char *dst, const char *src, size_t dstsize)
{
    size_t src_len = 0;
    while (src[src_len] != '\0') {
        src_len++;
    }

    if (dstsize == 0) {
        return src_len;
    }

    size_t copy_len = src_len;
    if (copy_len >= dstsize) {
        copy_len = dstsize - 1;
    }

    for (size_t i = 0; i < copy_len; i++) {
        dst[i] = src[i];
    }
    dst[copy_len] = '\0';

    return src_len;
}

size_t bb_str_field(char *dst, const char *src, size_t dstsize)
{
    size_t src_len = 0;
    while (src[src_len] != '\0') {
        src_len++;
    }

    if (dstsize == 0) {
        return src_len;
    }

    size_t copy_len = src_len;
    if (copy_len > dstsize) {
        copy_len = dstsize;
    }

    for (size_t i = 0; i < copy_len; i++) {
        dst[i] = src[i];
    }

    for (size_t i = copy_len; i < dstsize; i++) {
        dst[i] = '\0';
    }

    return src_len;
}
