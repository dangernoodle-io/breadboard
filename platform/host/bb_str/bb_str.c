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

static int bb_str_hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

size_t bb_str_hex_to_bytes(const char *hex, uint8_t *out, size_t max_out)
{
    if (!hex) return 0;

    size_t n = 0;
    while (n < max_out) {
        int hi = bb_str_hex_nibble(hex[0]);
        if (hi < 0) break;
        int lo = bb_str_hex_nibble(hex[1]);
        if (lo < 0) break;

        out[n] = (uint8_t)((hi << 4) | lo);
        n++;
        hex += 2;
    }

    return n;
}

static char bb_str_hex_digit(uint8_t nibble)
{
    static const char digits[] = "0123456789abcdef";
    return digits[nibble & 0x0f];
}

size_t bb_str_bytes_to_hex(const uint8_t *data, size_t len, char *hex, size_t hex_cap)
{
    if (hex_cap == 0) return 0;

    size_t max_pairs = (hex_cap - 1) / 2;
    size_t n = (len < max_pairs) ? len : max_pairs;

    for (size_t i = 0; i < n; i++) {
        hex[i * 2]     = bb_str_hex_digit((uint8_t)(data[i] >> 4));
        hex[i * 2 + 1] = bb_str_hex_digit(data[i] & 0x0f);
    }
    hex[n * 2] = '\0';

    return n;
}

// Locate the byte just after `"key":` in payload (whitespace-tolerant around
// the colon), or NULL if key is absent.
static const char *bb_str_envelope_find_key_value(const char *payload, const char *end, const char *key)
{
    size_t key_len = strlen(key);
    for (const char *q = payload; q + key_len <= end; q++) {
        if (memcmp(q, key, key_len) == 0) {
            const char *v = q + key_len;
            while (v < end && (*v == ' ' || *v == '\t' || *v == '\n' || *v == '\r')) v++;
            return v;
        }
    }
    return NULL;
}

bool bb_str_envelope_split(const char *payload, int len,
                            const char **ts_start, size_t *ts_len,
                            const char **data_start, size_t *data_len)
{
    if (!payload || len <= 0 || !ts_start || !ts_len || !data_start || !data_len) return false;

    const char *end = payload + len;

    const char *ts_v = bb_str_envelope_find_key_value(payload, end, "\"ts_ms\":");
    if (!ts_v) return false;
    const char *ts_end = ts_v;
    if (ts_end < end && *ts_end == '-') ts_end++;
    while (ts_end < end && *ts_end >= '0' && *ts_end <= '9') ts_end++;
    if (ts_end == ts_v || (ts_end == ts_v + 1 && *ts_v == '-')) return false;

    const char *data_v = bb_str_envelope_find_key_value(payload, end, "\"data\":");
    if (!data_v || data_v >= end || *data_v != '{') return false;
    int depth = 0;
    bool in_str = false;
    const char *p = data_v;
    while (p < end) {
        if (in_str) {
            if (*p == '\\') {
                // Escaped char (\" or \\ or any other) — skip both bytes so
                // an escaped quote never toggles in_str and an escaped
                // backslash is never mistaken for the start of a further
                // escape sequence.
                p += 2;
                continue;
            }
            if (*p == '"') in_str = false;
        } else {
            if (*p == '"') in_str = true;
            else if (*p == '{') depth++;
            else if (*p == '}') {
                depth--;
                if (depth == 0) { p++; break; }
            }
        }
        p++;
    }
    if (depth != 0) return false;

    *ts_start   = ts_v;
    *ts_len     = (size_t)(ts_end - ts_v);
    *data_start = data_v;
    *data_len   = (size_t)(p - data_v);
    return true;
}
