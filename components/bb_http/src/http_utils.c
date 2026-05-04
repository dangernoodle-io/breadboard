#include "bb_http.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int istrcmp(const char *a, const char *b)
{
    while (*a && *b) {
        int da = tolower((unsigned char)*a++);
        int db = tolower((unsigned char)*b++);
        if (da != db) return da - db;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

bool bb_url_parse_bool(const char *val, bool *out)
{
    if (!val || val[0] == '\0') return false;
    static const char *truthy[] = { "1", "true", "on", "yes", "t", "y", NULL };
    static const char *falsy[]  = { "0", "false", "off", "no", "f", "n", NULL };
    for (const char **p = truthy; *p; p++) {
        if (istrcmp(val, *p) == 0) { *out = true; return true; }
    }
    for (const char **p = falsy; *p; p++) {
        if (istrcmp(val, *p) == 0) { *out = false; return true; }
    }
    return false;
}

bool bb_url_parse_uint(const char *val, unsigned long *out)
{
    if (!val || val[0] == '\0') return false;
    // Reject anything that isn't a pure decimal digit run — strtoul would
    // happily accept "12abc" or whitespace; we want strict. After this
    // check the only remaining failure mode is overflow (ERANGE).
    for (const char *p = val; *p; p++) {
        if (*p < '0' || *p > '9') return false;
    }
    errno = 0;
    unsigned long v = strtoul(val, NULL, 10);
    if (errno == ERANGE) return false;
    *out = v;
    return true;
}

// Extract a field from URL-encoded body: "field=value&..."
// Handles %XX decoding and + as space
void bb_url_decode_field(const char *body, const char *field, char *out, size_t out_size)
{
    out[0] = '\0';
    // Find "field=" in body
    char key[64];
    snprintf(key, sizeof(key), "%s=", field);
    const char *start = strstr(body, key);
    if (!start) return;
    start += strlen(key);
    const char *end = strchr(start, '&');
    if (!end) end = start + strlen(start);

    size_t i = 0;
    while (start < end && i < out_size - 1) {
        if (*start == '+') {
            out[i++] = ' ';
            start++;
        } else if (*start == '%' && start + 2 < end) {
            char hex[3] = { start[1], start[2], '\0' };
            out[i++] = (char)strtoul(hex, NULL, 16);
            start += 3;
        } else {
            out[i++] = *start++;
        }
    }
    out[i] = '\0';
}
