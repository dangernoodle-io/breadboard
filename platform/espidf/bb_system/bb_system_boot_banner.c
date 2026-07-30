// CONFIG_BB_SYSTEM_BOOT_BANNER (components/bb_system/Kconfig): emits a
// one-time boot banner naming project/version/build/idf via the existing
// bb_system accessors. Lives in bb_system because it uses only bb_system
// accessors; bb_system already PRIV_REQUIRES bb_log.
#include "bb_system.h"
#include "bb_log.h"

static const char *TAG = "bb_system_boot";

// Worst-case line length (bb_system_boot_banner_format's literal text +
// esp_app_desc_t's max field widths + the longest possible reset-reason
// rendering), rounded up well above the computed worst case rather than
// pinned exactly to it:
//   literal "project=" + " version=" + " build=" + " " + " idf=" + " reset="
//     = 8 + 9 + 7 + 1 + 5 + 7 = 37
//   project_name[32]/version[32]/idf_ver[32] -> 31 usable chars each = 93
//   date[16]/time[16] -> 15 usable chars each = 30
//   reset reason worst case: bb_system_reset_reason_format's unmapped
//     rendering, "unknown(-2147483648)" = 20 chars (longer than any mapped
//     string, e.g. "deep_sleep" at 10)
//   37 + 93 + 30 + 20 + 1 (NUL) = 181
#define BOOT_BANNER_LINE_MAX 224

bb_err_t bb_system_boot_banner_init(void)
{
    char line[BOOT_BANNER_LINE_MAX];

    // Hardware reset reason (bb_system_get_reset_reason -> esp_reset_reason()):
    // classifies watchdog/panic/power-on/... and needs no NVS/storage, so it
    // is always present even on a blank-NVS board where the banner is the
    // only diagnostic surface available (see B1-1273). Rendered via
    // bb_system_reset_reason_format rather than bb_system_reset_reason_str
    // directly so an unmapped/future reset cause preserves its raw numeric
    // value (e.g. "unknown(12)") instead of collapsing indistinguishably
    // into a bare "unknown".
    char reset_reason[BB_SYSTEM_RESET_REASON_STR_MAX];
    bb_system_reset_reason_format(bb_system_get_reset_reason(),
                                   bb_system_get_reset_reason_raw(),
                                   reset_reason, sizeof(reset_reason));

    int n = bb_system_boot_banner_format(line, sizeof(line),
                                          bb_system_get_project_name(),
                                          bb_system_get_version(),
                                          bb_system_get_build_date(),
                                          bb_system_get_build_time(),
                                          bb_system_get_idf_version(),
                                          reset_reason);
    if (n < 0) {
        bb_log_w(TAG, "boot banner format failed");
        return BB_OK;
    }

    // line is always NUL-terminated within sizeof(line) even if n >= sizeof(line)
    // (truncated) — safe to log as-is either way.
    bb_log_i(TAG, "%s", line);

    // The console writer normally drains asynchronously on a low-priority
    // background task (bb_log_stream_init) -- on a reset shortly after boot
    // (the exact failure mode this banner exists to diagnose, see B1-1273),
    // that task may never get scheduled before the reset, silently dropping
    // this line. Flush synchronously so the banner (including reset=) is on
    // the wire before this call returns, independent of init ordering versus
    // bb_log_stream_init. A timeout here means we can no longer be sure of
    // that -- surface it rather than silently discard it (bb_log_flush_
    // timeouts() also lets a caller notice this after the fact).
    if (bb_log_flush() != BB_OK) {
        bb_log_w(TAG, "boot banner flush timed out -- banner may not be on the wire yet");
    }
    return BB_OK;
}
