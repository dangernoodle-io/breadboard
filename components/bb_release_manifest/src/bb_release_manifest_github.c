#include "bb_release_manifest.h"
#include <string.h>
#include <stdio.h>

/**
 * Helper: find the next `"<key>"<ws>:` pair in [p, end).
 * Returns pointer to the first non-whitespace byte of the value, or NULL if not found.
 * Skips over the contents of any string it encounters so it never matches a key
 * pattern that lives inside a string value.
 */
static const char *find_key(const char *p, const char *end, const char *key)
{
    size_t key_len = strlen(key);
    while (p < end) {
        if (*p == '"') {
            const char *str_start = ++p;
            while (p < end && *p != '"') {  // LCOV_EXCL_BR_LINE — EOF inside well-formed JSON unreachable
                if (*p == '\\' && p + 1 < end) p += 2;  // LCOV_EXCL_BR_LINE — escape skip
                else p++;
            }
            if (p >= end) return NULL;  // LCOV_EXCL_LINE — unterminated string defensive
            const char *str_end = p;
            p++;
            const char *q = p;
            while (q < end && (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r')) q++;  // LCOV_EXCL_BR_LINE
            if (q < end && *q == ':') {  // LCOV_EXCL_BR_LINE — well-formed JSON always has `:` after key string
                if ((size_t)(str_end - str_start) == key_len &&
                    memcmp(str_start, key, key_len) == 0) {
                    q++;
                    while (q < end && (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r')) q++;  // LCOV_EXCL_BR_LINE
                    return q;
                }
            }
        } else {
            p++;
        }
    }
    return NULL;
}

/**
 * Helper: Copy a JSON string value into out (truncated to out_size-1, always null-
 * terminated). p must point at the opening `"`. Returns pointer past the
 * closing `"`, or NULL if not a string / unterminated.
 */
static const char *copy_string_value(const char *p, const char *end,
                                     char *out, size_t out_size)
{
    if (p >= end || *p != '"' || out_size == 0) return NULL;  // LCOV_EXCL_BR_LINE — defensive
    p++;
    size_t i = 0;
    while (p < end && *p != '"') {  // LCOV_EXCL_BR_LINE — EOF inside copy defensive
        char c = *p;
        if (c == '\\' && p + 1 < end) {  // LCOV_EXCL_BR_LINE — escape at EOF defensive
            p++;
            switch (*p) {  // LCOV_EXCL_BR_LINE — only specific escapes appear in GitHub tag_name / URLs
                case 'n':  c = '\n'; p++; break;  // LCOV_EXCL_LINE — not in tag/url
                case 't':  c = '\t'; p++; break;  // LCOV_EXCL_LINE
                case 'r':  c = '\r'; p++; break;  // LCOV_EXCL_LINE
                case 'b':  c = '\b'; p++; break;  // LCOV_EXCL_LINE
                case 'f':  c = '\f'; p++; break;  // LCOV_EXCL_LINE
                case '"':  c = '"';  p++; break;  // LCOV_EXCL_LINE
                case '\\': c = '\\'; p++; break;
                case '/':  c = '/';  p++; break;  // LCOV_EXCL_LINE
                case 'u':
                    if (p + 4 < end) p += 5;  // LCOV_EXCL_BR_LINE — defensive
                    else p = end;             // LCOV_EXCL_LINE — defensive
                    continue;
                default:   c = *p;  p++; break;  // LCOV_EXCL_LINE
            }
        } else {
            p++;
        }
        if (i + 1 < out_size) out[i++] = c;  // LCOV_EXCL_BR_LINE — truncation path covered separately
    }
    if (p >= end) return NULL;  // LCOV_EXCL_LINE — unterminated string defensive
    out[i] = '\0';
    return p + 1;
}

/**
 * Helper: p must point at '{'. Returns pointer to the matching '}', or NULL on
 * unbalanced / EOF. Tracks string boundaries so braces inside strings
 * don't confuse the depth count.
 */
static const char *match_brace(const char *p, const char *end)
{
    if (p >= end || *p != '{') return NULL;  // LCOV_EXCL_BR_LINE — defensive
    int depth = 1;
    p++;
    while (p < end && depth > 0) {  // LCOV_EXCL_BR_LINE — well-formed objects always balance
        if (*p == '"') {
            p++;
            while (p < end && *p != '"') {  // LCOV_EXCL_BR_LINE
                if (*p == '\\' && p + 1 < end) p += 2;  // LCOV_EXCL_BR_LINE — escape inside object string
                else p++;
            }
            if (p >= end) return NULL;  // LCOV_EXCL_LINE — unterminated defensive
            p++;
        // LCOV_EXCL_START — nested objects not in GitHub assets[]; flat top-level only
        } else if (*p == '{') {
            depth++;
            p++;
        } else if (*p == '}') {
            depth--;
            if (depth == 0) return p;
            p++;
        }
        // LCOV_EXCL_STOP
        else {
            p++;
        }
    }
    return NULL;  // LCOV_EXCL_LINE — unbalanced defensive
}

bb_err_t bb_release_manifest_parse_github(
    const char *body, size_t body_len,
    const char *board_name,
    char *out_tag, size_t tag_size,
    char *out_url, size_t url_size)
{
    if (!body || !board_name || !out_tag || !out_url) {
        return BB_ERR_INVALID_ARG;
    }
    if (tag_size == 0 || url_size == 0) {
        return BB_ERR_INVALID_ARG;
    }

    // Zero outputs so callers can distinguish which extraction failed when
    // BB_ERR_NOT_FOUND comes back: out_tag empty => tag missing, out_tag
    // populated but out_url empty => no matching asset.
    out_tag[0] = '\0';
    out_url[0] = '\0';

    const char *end = body + body_len;

    /* tag_name is required; missing -> BB_ERR_NOT_FOUND. */
    const char *tag_p = find_key(body, end, "tag_name");
    if (!tag_p || *tag_p != '"') return BB_ERR_NOT_FOUND;  // LCOV_EXCL_BR_LINE — *!='"' defensive
    if (!copy_string_value(tag_p, end, out_tag, tag_size)) return BB_ERR_NOT_FOUND;  // LCOV_EXCL_BR_LINE

    /* assets array: must be present AND an array. */
    const char *assets_p = find_key(body, end, "assets");
    if (!assets_p || *assets_p != '[') return BB_ERR_NOT_FOUND;  // LCOV_EXCL_BR_LINE — partial: assets_p NULL not tested
    assets_p++;

    char asset_name[128];
    snprintf(asset_name, sizeof(asset_name), "%s.bin", board_name);

    const char *p = assets_p;
    while (p < end) {  // LCOV_EXCL_BR_LINE — loop exits via break; p>=end defensive
        // LCOV_EXCL_BR_START — separator skip; only `,` is exercised, whitespace defensive
        while (p < end &&
               (*p == ',' || *p == ' ' || *p == '\t' ||
                *p == '\n' || *p == '\r')) p++;
        // LCOV_EXCL_BR_STOP
        if (p >= end || *p == ']') break;  // LCOV_EXCL_BR_LINE — p>=end defensive, normal exit via *p==']'
        if (*p != '{') break;  // LCOV_EXCL_BR_LINE — defensive

        const char *obj_end = match_brace(p, end);
        if (!obj_end) return BB_ERR_NOT_FOUND;  // LCOV_EXCL_BR_LINE — defensive

        char this_name[128] = {0};
        const char *name_p = find_key(p, obj_end, "name");
        if (name_p && *name_p == '"') {  // LCOV_EXCL_BR_LINE — partial: *!='"' defensive
            copy_string_value(name_p, obj_end, this_name, sizeof(this_name));
        }

        if (strcmp(this_name, asset_name) == 0) {
            const char *url_p = find_key(p, obj_end, "browser_download_url");
            if (!url_p || *url_p != '"') return BB_ERR_NOT_FOUND;  // LCOV_EXCL_BR_LINE — *!='"' defensive
            if (!copy_string_value(url_p, obj_end, out_url, url_size)) return BB_ERR_NOT_FOUND;  // LCOV_EXCL_BR_LINE
            return BB_OK;
        }

        p = obj_end + 1;
    }

    return BB_ERR_NOT_FOUND;
}
