#include "bb_mdns.h"
#include <string.h>
#include <ctype.h>

/*
 * Sanitize a string into RFC 1035 hostname label form using a single
 * work buffer processed in-place:
 *   1. Sanitize: lowercase alphanumeric pass-through, everything else -> '-'
 *   2. Collapse consecutive '-' (tracked with write pointer)
 *   3. Trim leading/trailing '-'
 *
 * Returns the number of characters written to work[] (excluding NUL).
 * work must be at least work_size bytes; src may be NULL (produces empty output).
 */
static size_t sanitize_into(const char *src, char *work, size_t work_size)
{
    size_t w = 0;

    /* Pass 1: sanitize + collapse '-' in one scan */
    if (src) {
        for (const char *p = src; *p && w < work_size - 1; p++) {
            char c = *p;
            char out;
            if (c >= 'a' && c <= 'z') {
                out = c;
            } else if (c >= 'A' && c <= 'Z') {
                out = (char)(c - ('A' - 'a'));
            } else if (c >= '0' && c <= '9') {
                out = c;
            } else {
                out = '-';
            }
            /* Collapse consecutive '-' */
            if (out == '-' && w > 0 && work[w - 1] == '-') {
                continue;
            }
            work[w++] = out;
        }
    }
    work[w] = '\0';

    /* Pass 2: trim leading '-' by advancing start pointer in-place */
    size_t start = 0;
    while (start < w && work[start] == '-') start++;

    /* Pass 3: trim trailing '-' */
    size_t end = w;
    while (end > start && work[end - 1] == '-') end--;

    size_t final_len = end - start;
    if (start > 0 && final_len > 0) {
        memmove(work, work + start, final_len);
    }
    work[final_len] = '\0';
    return final_len;
}

/* Sanitize and build RFC 1035-compliant hostname label. */
void bb_mdns_build_hostname(const char *prefix, const char *suffix, char *out, size_t out_size)
{
    if (out_size < 1) return;

    size_t max_label = (out_size > 63) ? 63 : (out_size - 1);

    /* Prefix always processed first */
    char work[256];
    size_t prefix_len = sanitize_into(prefix, work, sizeof(work));
    if (prefix_len > max_label) prefix_len = max_label;

    if (!suffix || !*suffix) {
        /* No suffix: output is just the sanitized prefix */
        if (prefix_len > 0) memcpy(out, work, prefix_len);
        out[prefix_len] = '\0';
        return;
    }

    /* Save sanitized prefix before reusing work[] for suffix */
    char prefix_buf[64]; /* label max is 63 chars */
    if (prefix_len > 0) memcpy(prefix_buf, work, prefix_len);
    prefix_buf[prefix_len] = '\0';

    /* Sanitize suffix into the same work buffer */
    size_t suffix_len = sanitize_into(suffix, work, sizeof(work));

    /* Build "prefix-suffix" capped at max_label */
    size_t sep = (prefix_len > 0 && suffix_len > 0) ? 1 : 0;
    size_t avail_suffix = (prefix_len + sep < max_label) ? (max_label - prefix_len - sep) : 0;
    if (suffix_len > avail_suffix) suffix_len = avail_suffix;

    size_t pos = 0;
    if (prefix_len > 0) {
        memcpy(&out[pos], prefix_buf, prefix_len);
        pos += prefix_len;
    }
    if (sep && suffix_len > 0) {
        out[pos++] = '-';
    }
    if (suffix_len > 0) {
        memcpy(&out[pos], work, suffix_len);
        pos += suffix_len;
    }
    out[pos] = '\0';
}
