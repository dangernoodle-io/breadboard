// bb_str — portable string-safety helpers.
//
// Compiled on both host (tests) and ESP-IDF. Pure C, no platform deps.

#include "bb_str.h"

#include <stdbool.h>
#include <string.h>

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

static bool bb_str_kv_is_space(char c)
{
    return c == ' ' || c == '\t';
}

void bb_str_kv_parse(const char *s, bb_str_kv_cb_t cb, void *ctx)
{
    if (!s || !cb) return;

    const char *p = s;
    while (*p) {
        const char *entry_start = p;
        const char *comma = strchr(p, ',');
        size_t entry_len = comma ? (size_t)(comma - p) : strlen(p);
        const char *entry_end = entry_start + entry_len;

        const char *eq = (const char *)memchr(entry_start, '=', entry_len);
        if (eq) {
            const char *key_s = entry_start;
            const char *key_e = eq;
            const char *val_s = eq + 1;
            const char *val_e = entry_end;

            while (key_s < key_e && bb_str_kv_is_space(*key_s)) key_s++;
            while (key_e > key_s && bb_str_kv_is_space(key_e[-1])) key_e--;
            while (val_s < val_e && bb_str_kv_is_space(*val_s)) val_s++;
            while (val_e > val_s && bb_str_kv_is_space(val_e[-1])) val_e--;

            if (key_e > key_s) {
                cb(key_s, (size_t)(key_e - key_s), val_s, (size_t)(val_e - val_s), ctx);
            }
        }

        if (!comma) break;
        p = comma + 1;
    }
}
