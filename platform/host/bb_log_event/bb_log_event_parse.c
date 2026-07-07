// bb_log_event_parse — pure host+espidf log-line parser for bb_log_event.
// No FreeRTOS or ESP-IDF dependencies; host tests cover all branches.

#include "bb_log_event_parse.h"
#include <string.h>
#include <stddef.h>

// Strip a leading ANSI CSI escape sequence of the form ESC [ ... m
// Returns pointer past the sequence, or the original pointer if none found.
static const char *strip_ansi(const char *p, size_t *rem)
{
    if (*rem < 3) return p;
    if ((unsigned char)p[0] != 0x1B || p[1] != '[') return p;
    size_t i = 2;
    while (i < *rem && p[i] != 'm') i++;
    if (i >= *rem) return p;  // no closing 'm' — don't strip
    i++;  // skip 'm'
    *rem -= i;
    return p + i;
}

// Strip trailing CR/LF
static size_t rtrim_crlf(const char *s, size_t len)
{
    while (len > 0 && (s[len - 1] == '\r' || s[len - 1] == '\n'))
        len--;
    return len;
}

// Copy at most (cap-1) bytes and NUL-terminate.
static void safe_copy(char *dst, const char *src, size_t n, size_t cap)
{
    if (cap == 0) return;
    size_t copy = (n < cap - 1) ? n : cap - 1;
    memcpy(dst, src, copy);
    dst[copy] = '\0';
}

void bb_log_event_parse(const char *line, size_t len,
                        char *level_out,
                        char *tag_out, size_t tag_cap,
                        char *msg_out, size_t msg_cap)
{
    // Default fallback values
    if (level_out) *level_out = '?';
    if (tag_out && tag_cap > 0) tag_out[0] = '\0';
    if (msg_out && msg_cap > 0) msg_out[0] = '\0';

    if (!line || len == 0) return;

    // Strip trailing newlines first
    len = rtrim_crlf(line, len);
    if (len == 0) return;

    // Strip leading ANSI escape sequence if present
    const char *p = strip_ansi(line, &len);

    // Expect: "<L> (<ts>) <tag>: <msg>"
    // Minimum: "I (1) t: m" = 10 chars
    if (len < 6) goto fallback;

    // Check level char is one of I W E D V and next char is ' '
    char lv = p[0];
    if (lv != 'I' && lv != 'W' && lv != 'E' && lv != 'D' && lv != 'V') goto fallback;
    if (p[1] != ' ' || p[2] != '(') goto fallback;

    // Find closing ')' of the timestamp field
    const char *close_paren = memchr(p + 3, ')', len - 3);
    if (!close_paren) goto fallback;

    // After ") " comes the tag
    const char *after_ts = close_paren + 1;
    size_t after_ts_len = len - (size_t)(after_ts - p);
    if (after_ts_len < 2 || after_ts[0] != ' ') goto fallback;

    const char *tag_start = after_ts + 1;
    size_t tag_region_len = after_ts_len - 1;

    // Find ": " separator between tag and message
    const char *colon = NULL;
    for (size_t i = 0; i + 1 < tag_region_len; i++) {
        if (tag_start[i] == ':' && tag_start[i + 1] == ' ') {
            colon = tag_start + i;
            break;
        }
    }
    if (!colon) goto fallback;

    // Parsed successfully
    if (level_out) *level_out = lv;

    size_t tag_len = (size_t)(colon - tag_start);
    if (tag_out) safe_copy(tag_out, tag_start, tag_len, tag_cap);

    const char *msg_start = colon + 2;
    size_t msg_len = (size_t)((p + len) - msg_start);
    // Bound msg to 160 bytes before serialization
    if (msg_len > 160) msg_len = 160;
    if (msg_out) safe_copy(msg_out, msg_start, msg_len, msg_cap);
    return;

fallback:
    // level="?", tag="", msg=entire trimmed line
    if (level_out) *level_out = '?';
    if (tag_out && tag_cap > 0) tag_out[0] = '\0';
    if (msg_out) safe_copy(msg_out, p, len, msg_cap);
}
