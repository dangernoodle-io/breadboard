// Shared host-common helpers -- pure C, no backend-specific JSON tree
// dependency. The bb_json_t tree API itself lives in the platform-specific
// backend:
//   platform/espidf/bb_json/bb_json_cjson.c  (ESP-IDF + host via cJSON)
//   platform/arduino/bb_json/bb_json_arduinojson.cpp (Arduino via ArduinoJson)

#include "bb_json.h"

#include <string.h>

// Locate the byte just after `"key":` in payload (whitespace-tolerant around
// the colon), or NULL if key is absent.
static const char *find_key_value(const char *payload, const char *end, const char *key)
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

bool bb_json_envelope_split(const char *payload, int len,
                             const char **ts_start, size_t *ts_len,
                             const char **data_start, size_t *data_len)
{
    if (!payload || len <= 0 || !ts_start || !ts_len || !data_start || !data_len) return false;

    const char *end = payload + len;

    const char *ts_v = find_key_value(payload, end, "\"ts_ms\":");
    if (!ts_v) return false;
    const char *ts_end = ts_v;
    if (ts_end < end && *ts_end == '-') ts_end++;
    while (ts_end < end && *ts_end >= '0' && *ts_end <= '9') ts_end++;
    if (ts_end == ts_v || (ts_end == ts_v + 1 && *ts_v == '-')) return false;

    const char *data_v = find_key_value(payload, end, "\"data\":");
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
