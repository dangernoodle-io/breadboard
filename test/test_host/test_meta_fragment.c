#include "test_meta_fragment.h"

#include <string.h>

bool test_meta_fragment_extract_properties(const char *schema, char *out, size_t out_size)
{
    if (!schema || !out || out_size == 0) return false;

    static const char k_key[] = "\"properties\":";
    const char *start = strstr(schema, k_key);
    if (!start) return false;

    const char *brace = start + (sizeof(k_key) - 1);
    if (*brace != '{') return false;

    int         depth  = 0;
    bool        in_str = false;
    const char *end    = NULL;

    for (const char *p = brace; *p; p++) {
        char c = *p;

        if (in_str) {
            if (c == '\\' && p[1] != '\0') { p++; continue; }
            if (c == '"') in_str = false;
            continue;
        }

        if (c == '"') { in_str = true; continue; }
        if (c == '{') { depth++; continue; }
        if (c == '}') {
            depth--;
            if (depth == 0) { end = p; break; }
        }
    }
    if (!end) return false;

    size_t len = (size_t)(end - start) + 1;
    if (len + 1 > out_size) return false;

    memcpy(out, start, len);
    out[len] = '\0';
    return true;
}
