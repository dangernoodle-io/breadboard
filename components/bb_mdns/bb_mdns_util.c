#include "bb_mdns.h"
#include <string.h>
#include <ctype.h>

// Sanitize and build RFC 1035-compliant hostname label.
void bb_mdns_build_hostname(const char *prefix, const char *suffix, char *out, size_t out_size)
{
    if (out_size < 1) {
        return;
    }

    // If suffix is NULL or empty, output is just the sanitized prefix.
    if (!suffix || !*suffix) {
        // Sanitize prefix: lowercase [a-z0-9], non-alphanumeric -> '-', collapse '-', trim edges
        char sanitized[256];
        size_t san_len = 0;

        if (prefix) {
            for (const char *p = prefix; *p && san_len < sizeof(sanitized) - 1; p++) {
                char c = *p;
                if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
                    sanitized[san_len++] = c;
                } else if (c >= 'A' && c <= 'Z') {
                    sanitized[san_len++] = c - ('A' - 'a');
                } else {
                    sanitized[san_len++] = '-';
                }
            }
        }
        sanitized[san_len] = '\0';

        // Collapse consecutive '-'
        char collapsed[256];
        size_t col_len = 0;
        for (size_t i = 0; i < san_len && col_len < sizeof(collapsed) - 1; i++) {
            if (sanitized[i] == '-' && col_len > 0 && collapsed[col_len - 1] == '-') {
                continue;
            }
            collapsed[col_len++] = sanitized[i];
        }
        collapsed[col_len] = '\0';

        // Trim leading and trailing '-'
        size_t trim_start = 0;
        while (trim_start < col_len && collapsed[trim_start] == '-') {
            trim_start++;
        }
        size_t trim_end = col_len;
        while (trim_end > trim_start && collapsed[trim_end - 1] == '-') {
            trim_end--;
        }

        size_t final_len = trim_end - trim_start;
        size_t max_len = (out_size > 63) ? 63 : (out_size - 1);
        if (final_len > max_len) {
            final_len = max_len;
        }

        if (final_len > 0) {
            memcpy(out, &collapsed[trim_start], final_len);
        }
        out[final_len] = '\0';
        return;
    }

    // Suffix is not empty; build "prefix-sanitized_suffix", capped at 63 chars total.

    // Sanitize prefix
    char prefix_sanitized[256];
    size_t prefix_san_len = 0;
    if (prefix) {
        for (const char *p = prefix; *p && prefix_san_len < sizeof(prefix_sanitized) - 1; p++) {
            char c = *p;
            if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
                prefix_sanitized[prefix_san_len++] = c;
            } else if (c >= 'A' && c <= 'Z') {
                prefix_sanitized[prefix_san_len++] = c - ('A' - 'a');
            } else {
                prefix_sanitized[prefix_san_len++] = '-';
            }
        }
    }
    prefix_sanitized[prefix_san_len] = '\0';

    // Collapse consecutive '-' in prefix
    char prefix_collapsed[256];
    size_t prefix_col_len = 0;
    for (size_t i = 0; i < prefix_san_len && prefix_col_len < sizeof(prefix_collapsed) - 1; i++) {
        if (prefix_sanitized[i] == '-' && prefix_col_len > 0 && prefix_collapsed[prefix_col_len - 1] == '-') {
            continue;
        }
        prefix_collapsed[prefix_col_len++] = prefix_sanitized[i];
    }
    prefix_collapsed[prefix_col_len] = '\0';

    // Trim leading and trailing '-' in prefix
    size_t prefix_trim_start = 0;
    while (prefix_trim_start < prefix_col_len && prefix_collapsed[prefix_trim_start] == '-') {
        prefix_trim_start++;
    }
    size_t prefix_trim_end = prefix_col_len;
    while (prefix_trim_end > prefix_trim_start && prefix_collapsed[prefix_trim_end - 1] == '-') {
        prefix_trim_end--;
    }
    size_t prefix_final_len = prefix_trim_end - prefix_trim_start;

    // Sanitize suffix
    char suffix_sanitized[256];
    size_t suffix_san_len = 0;
    for (const char *p = suffix; *p && suffix_san_len < sizeof(suffix_sanitized) - 1; p++) {
        char c = *p;
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            suffix_sanitized[suffix_san_len++] = c;
        } else if (c >= 'A' && c <= 'Z') {
            suffix_sanitized[suffix_san_len++] = c - ('A' - 'a');
        } else {
            suffix_sanitized[suffix_san_len++] = '-';
        }
    }
    suffix_sanitized[suffix_san_len] = '\0';

    // Collapse consecutive '-' in suffix
    char suffix_collapsed[256];
    size_t suffix_col_len = 0;
    for (size_t i = 0; i < suffix_san_len && suffix_col_len < sizeof(suffix_collapsed) - 1; i++) {
        if (suffix_sanitized[i] == '-' && suffix_col_len > 0 && suffix_collapsed[suffix_col_len - 1] == '-') {
            continue;
        }
        suffix_collapsed[suffix_col_len++] = suffix_sanitized[i];
    }
    suffix_collapsed[suffix_col_len] = '\0';

    // Trim leading and trailing '-' in suffix
    size_t suffix_trim_start = 0;
    while (suffix_trim_start < suffix_col_len && suffix_collapsed[suffix_trim_start] == '-') {
        suffix_trim_start++;
    }
    size_t suffix_trim_end = suffix_col_len;
    while (suffix_trim_end > suffix_trim_start && suffix_collapsed[suffix_trim_end - 1] == '-') {
        suffix_trim_end--;
    }
    size_t suffix_final_len = suffix_trim_end - suffix_trim_start;

    // Build result: prefix + "-" + suffix, capped at 63 chars RFC 1035 label max
    size_t max_len = (out_size > 63) ? 63 : (out_size - 1);
    size_t sep_len = 1; // "-"
    size_t available_suffix = (prefix_final_len + sep_len < max_len) ? (max_len - prefix_final_len - sep_len) : 0;

    if (suffix_final_len > available_suffix) {
        suffix_final_len = available_suffix;
    }

    size_t pos = 0;
    if (prefix_final_len > 0) {
        memcpy(&out[pos], &prefix_collapsed[prefix_trim_start], prefix_final_len);
        pos += prefix_final_len;
    }

    if (suffix_final_len > 0) {
        out[pos++] = '-';
        memcpy(&out[pos], &suffix_collapsed[suffix_trim_start], suffix_final_len);
        pos += suffix_final_len;
    }

    out[pos] = '\0';
}
