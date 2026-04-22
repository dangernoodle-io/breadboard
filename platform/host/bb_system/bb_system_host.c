#include "bb_system.h"

#include "bb_log.h"

static const char *TAG = "bb_system";

bb_reset_reason_t bb_system_get_reset_reason(void)
{
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
    return false;
}

void bb_system_log_boot_info(void)
{
    bb_log_i(TAG, "boot: reset=power-on");
}
