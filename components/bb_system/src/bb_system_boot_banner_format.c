#include "bb_system.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

bool bb_system_reset_reason_format(bb_reset_reason_t r, int32_t raw, char *out, size_t out_len)
{
    if (!out || out_len == 0) return false;

    // A reason is "known"/mapped iff it falls strictly between the
    // UNKNOWN sentinel (0) and the trailing COUNT sentinel -- every real
    // X-macro entry in BB_RESET_REASON_LIST occupies exactly that
    // contiguous range (see the enum in bb_system.h), so this is a direct
    // range check against the enum itself rather than an implicit coupling
    // to bb_system_reset_reason_str()'s string output (which would break
    // silently if that function's "unknown" literal ever changed).
    bool is_known = r > BB_RESET_REASON_UNKNOWN && r < BB_RESET_REASON_COUNT;
    if (is_known) {
        // Mapped reason: render exactly bb_system_reset_reason_str(r),
        // byte-for-byte -- raw is never consulted here.
        strncpy(out, bb_system_reset_reason_str(r), out_len - 1);
        out[out_len - 1] = '\0';
        return true;
    }

    // Unmapped/out-of-range: preserve the raw value rather than collapsing
    // to a bare "unknown" that can't distinguish one un-cataloged cause from
    // another.
    int n = snprintf(out, out_len, "unknown(%" PRId32 ")", raw);
    // n > 0 is unreachable-false via this fixed literal-prefixed format
    // string ("unknown(" alone guarantees a positive return) -- mirrors the
    // LCOV_EXCL_BR_LINE idiom used for the equivalent defensive snprintf
    // check in bb_system_reboot_record.c.
    return n > 0 && (size_t)n < out_len;  // LCOV_EXCL_BR_LINE — n>0 unreachable-false, see comment above
}

int bb_system_boot_banner_format(char *out, size_t out_len,
                                  const char *project, const char *version,
                                  const char *build_date, const char *build_time,
                                  const char *idf_version, const char *reset_reason)
{
    if (!out || out_len == 0) return -1;

    project = project ? project : "?";
    version = version ? version : "?";
    build_date = build_date ? build_date : "?";
    build_time = build_time ? build_time : "?";
    idf_version = idf_version ? idf_version : "?";
    reset_reason = reset_reason ? reset_reason : "?";

    return snprintf(out, out_len, "project=%s version=%s build=%s %s idf=%s reset=%s",
                     project, version, build_date, build_time, idf_version, reset_reason);
}
