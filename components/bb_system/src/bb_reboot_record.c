// Pure (host-testable) reboot-reason record pack/unpack + wire-string mapping.
// No ESP-IDF or FreeRTOS types here — compiled on host, ESP-IDF, and Arduino.
#include "bb_system.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

// Compile-time guard: BB_RESET_SRC_LIST must have exactly one X() entry per
// mapped bb_reset_source_t value (every enumerator except the UNKNOWN
// default-case sentinel and the trailing __COUNT sentinel). Mirrors the
// BB_RESET_REASON_LIST guard in the espidf/host/arduino bb_system.c files.
#define BB_RESET_SRC_LIST_COUNT_ONE(v, s) +1
_Static_assert((0 BB_RESET_SRC_LIST(BB_RESET_SRC_LIST_COUNT_ONE)) == (BB_RESET_SRC__COUNT - 1),
               "BB_RESET_SRC_LIST entry count must match bb_reset_source_t cardinality (excluding UNKNOWN)");
#undef BB_RESET_SRC_LIST_COUNT_ONE

const char *bb_reset_source_str(bb_reset_source_t src)
{
    switch (src) {
#define X(v, s) case v: return s;
        BB_RESET_SRC_LIST(X)
#undef X
        case BB_RESET_SRC_UNKNOWN:
        default:
            return "unknown";
    }
}

bool bb_reboot_record_encode(const bb_reboot_record_t *r, char *buf, size_t buf_len)
{
    if (!r || !buf || buf_len == 0U) return false;

    // detail is freeform and placed last; truncate at the first '|' so the
    // fixed-field sscanf in bb_reboot_record_decode stays unambiguous. Bound
    // the scan to sizeof(r->detail) so an unterminated field (e.g. a future
    // caller that doesn't NUL-terminate) can never be scanned past.
    char safe_detail[sizeof(r->detail)];
    size_t dlen = strnlen(r->detail, sizeof(r->detail));
    const char *pipe = memchr(r->detail, '|', dlen);
    if (pipe) dlen = (size_t)(pipe - r->detail);
    // dlen == sizeof(r->detail) is reachable: an unterminated (no NUL within
    // the 49-byte array) detail field makes strnlen return the full length.
    if (dlen >= sizeof(safe_detail)) dlen = sizeof(safe_detail) - 1;
    memcpy(safe_detail, r->detail, dlen);
    safe_detail[dlen] = '\0';

    int n = snprintf(buf, buf_len, "%u|%" PRIu32 "|%" PRIu32 "|%s",
                      (unsigned)r->src, r->epoch_s, r->uptime_s, safe_detail);
    if (n < 0) return false;  // LCOV_EXCL_BR_LINE — snprintf of a numeric+string format never returns <0
    if ((size_t)n >= buf_len) return false;

    return true;
}

bool bb_reboot_record_decode(const char *str, bb_reboot_record_t *out)
{
    if (!str || !out) return false;

    // The src field must begin with a digit — sscanf's %u otherwise accepts
    // an optional leading sign/space (e.g. "-1"), which would silently
    // wrap into a huge unsigned value instead of being rejected outright.
    // (epoch_s/uptime_s sign-wrap via the same %u lenience is cosmetic-only
    // — a wrong timestamp, never a crash or loop — so it is not separately
    // guarded here. detail legitimately contains '-', e.g. "gw-unreachable",
    // so this guard is scoped to the src field only.)
    if (str[0] < '0' || str[0] > '9') return false;

    bb_reboot_record_t decoded;
    memset(&decoded, 0, sizeof(decoded));

    // src/epoch_s/uptime_s are parsed as unsigned into wider-than-needed
    // temps because scanf's %u has no direct uint32_t specifier portable
    // across libc; the valid ranges fit comfortably in unsigned on every
    // target this project builds for (mirrors bb_net_health's reboot-state
    // decode idiom).
    unsigned src_field = 0, epoch_field = 0, uptime_field = 0;
    int consumed = 0;
    if (sscanf(str, "%u|%u|%u|%n", &src_field, &epoch_field, &uptime_field, &consumed) != 3) {
        return false;
    }
    // consumed == 0 is reachable: a 3-field match with no trailing '|'
    // (e.g. "0|0|0") satisfies the three %u conversions but never reaches
    // the %n, since %n only fires after the literal '|' that follows it.
    if (consumed == 0) return false;
    if (src_field >= (unsigned)BB_RESET_SRC__COUNT) return false;

    const char *detail_start = str + consumed;
    size_t detail_len = strlen(detail_start);
    if (detail_len >= sizeof(decoded.detail)) detail_len = sizeof(decoded.detail) - 1;
    memcpy(decoded.detail, detail_start, detail_len);
    decoded.detail[detail_len] = '\0';

    decoded.src      = (uint8_t)src_field;
    decoded.epoch_s  = (uint32_t)epoch_field;
    decoded.uptime_s = (uint32_t)uptime_field;

    *out = decoded;
    return true;
}
