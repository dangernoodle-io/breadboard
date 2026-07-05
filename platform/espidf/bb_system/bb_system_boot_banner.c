// CONFIG_BB_SYSTEM_BOOT_BANNER (components/bb_system/Kconfig): auto-emit a
// one-time boot banner naming project/version/build/idf via the existing
// bb_system accessors. Lives in bb_system because it uses only bb_system
// accessors; bb_system already PRIV_REQUIRES both bb_log and bb_init.
// Self-registers at the EARLY tier, same shape as
// platform/espidf/bb_log/bb_log.c's bb_log_stream registration.
#include "bb_system.h"
#include "bb_log.h"
#include "bb_init.h"

static const char *TAG = "bb_system_boot";

static bb_err_t bb_system_boot_banner_init(void)
{
    char line[160];
    int n = bb_system_boot_banner_format(line, sizeof(line),
                                          bb_system_get_project_name(),
                                          bb_system_get_version(),
                                          bb_system_get_build_date(),
                                          bb_system_get_build_time(),
                                          bb_system_get_idf_version());
    if (n < 0) {
        bb_log_w(TAG, "boot banner format failed");
        return BB_OK;
    }

    // line is always NUL-terminated within sizeof(line) even if n >= sizeof(line)
    // (truncated) — safe to log as-is either way.
    bb_log_i(TAG, "%s", line);
    return BB_OK;
}

BB_INIT_REGISTER_EARLY(bb_system_boot_banner, bb_system_boot_banner_init);
