// Pure (host-testable) "key=value,key=value" string parser. No ESP-IDF or
// FreeRTOS types here — compiled on host, ESP-IDF, and Arduino.
#include "bb_kv.h"

#include <stdbool.h>
#include <string.h>

static bool is_kv_space(char c)
{
    return c == ' ' || c == '\t';
}

void bb_kv_parse(const char *s, bb_kv_cb_t cb, void *ctx)
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

            while (key_s < key_e && is_kv_space(*key_s)) key_s++;
            while (key_e > key_s && is_kv_space(key_e[-1])) key_e--;
            while (val_s < val_e && is_kv_space(*val_s)) val_s++;
            while (val_e > val_s && is_kv_space(val_e[-1])) val_e--;

            if (key_e > key_s) {
                cb(key_s, (size_t)(key_e - key_s), val_s, (size_t)(val_e - val_s), ctx);
            }
        }

        if (!comma) break;
        p = comma + 1;
    }
}
