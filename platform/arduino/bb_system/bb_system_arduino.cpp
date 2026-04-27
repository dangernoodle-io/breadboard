#include "bb_system.h"

#include "bb_log.h"

#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "bb_system";

bb_reset_reason_t bb_system_get_reset_reason(void)
{
    // Arduino doesn't provide reset reason; return best-effort guess
    return BB_RESET_REASON_POWERON;
}

const char *bb_system_reset_reason_str(bb_reset_reason_t r)
{
    switch (r) {
        case BB_RESET_REASON_POWERON:
            return "power-on";
        case BB_RESET_REASON_EXT:
            return "ext";
        case BB_RESET_REASON_SW:
            return "software";
        case BB_RESET_REASON_PANIC:
            return "panic";
        case BB_RESET_REASON_INT_WDT:
            return "int_wdt";
        case BB_RESET_REASON_TASK_WDT:
            return "task_wdt";
        case BB_RESET_REASON_WDT:
            return "wdt";
        case BB_RESET_REASON_DEEPSLEEP:
            return "deep_sleep";
        case BB_RESET_REASON_BROWNOUT:
            return "brownout";
        case BB_RESET_REASON_SDIO:
            return "sdio";
        case BB_RESET_REASON_UNKNOWN:
        default:
            return "unknown";
    }
}

bool bb_system_is_abnormal_reset(void)
{
    // Arduino doesn't provide reset reason information
    return false;
}

void bb_system_log_boot_info(void)
{
    bb_log_i(TAG, "boot: reset=power-on");
}

const char *bb_system_get_version(void)
{
#ifdef BB_SYSTEM_VERSION_OVERRIDE
    return BB_SYSTEM_VERSION_OVERRIDE;
#elif defined(BB_FIRMWARE_VERSION)
    return BB_FIRMWARE_VERSION;
#else
    return "0.0.0";
#endif
}

const char *bb_system_get_project_name(void)
{
    return "host";
}

const char *bb_system_get_build_date(void)
{
    return __DATE__;
}

const char *bb_system_get_build_time(void)
{
    return __TIME__;
}

const char *bb_system_get_idf_version(void)
{
    return "0.0.0-host";
}

void bb_system_restart(void)
{
    fprintf(stderr, "bb_system_restart: arduino stub — exiting\n");
    exit(0);
}
