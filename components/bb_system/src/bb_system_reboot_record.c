// Reboot-record persist helper (B1-532 PR1; relocated from bb_nv to
// bb_system, B1-750, bb_nv dissolution epic B1-708 — bb_system already owns
// the NVS namespace/keys (BB_REBOOT_NVS_NS/BB_REBOOT_KEY_LAST) and the
// reboot-reason SSOT). Portable — bb_nv_set_str and bb_reboot_record_encode
// are both already implemented identically on host and ESP-IDF, so this file
// compiles unchanged on every platform (mirrors bb_system_reboot_parse.c's
// and bb_system_boot_banner_format.c's placement: a portable helper living
// directly in components/bb_system/src/, not split under platform/{espidf,host}/).
#include "bb_system.h"

#include "bb_nv.h"
#include "bb_nv_namespaces.h"
#include "bb_nv_keys.h"
#include "bb_str.h"

#include <string.h>

bb_err_t bb_system_reboot_record_save(bb_reset_source_t src, const char *detail,
                                       uint32_t epoch_s, uint32_t uptime_s)
{
    bb_reboot_record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.src      = (uint8_t)src;
    rec.epoch_s  = epoch_s;
    rec.uptime_s = uptime_s;

    if (detail) {
        bb_strlcpy(rec.detail, detail, sizeof(rec.detail));
    }

    // BB_REBOOT_RECORD_STR_MAX (96) always fits the widest possible encoding
    // (3-digit src + 2x 10-digit uint32 + 48-char detail + delimiters/NUL =
    // 75 max), so the false branch is unreachable via this fixed-size call
    // site — mirrors the LCOV_EXCL_BR_LINE idiom used for the snprintf
    // branches in bb_reboot_record_encode itself.
    char buf[BB_REBOOT_RECORD_STR_MAX];
    if (!bb_reboot_record_encode(&rec, buf, sizeof(buf))) return BB_ERR_INVALID_ARG;  // LCOV_EXCL_BR_LINE — unreachable, see comment above

    return bb_nv_set_str(BB_REBOOT_NVS_NS, BB_REBOOT_KEY_LAST, buf);
}
