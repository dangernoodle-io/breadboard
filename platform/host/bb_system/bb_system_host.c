#include "bb_system.h"

#ifdef BB_SYSTEM_TESTING
#include "bb_system_test.h"
#endif

#include "bb_log.h"

#include <stdio.h>
#include <stdlib.h>

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

const char *bb_system_get_version(void)
{
#ifdef BB_SYSTEM_VERSION_OVERRIDE
    return BB_SYSTEM_VERSION_OVERRIDE;
#else
    return "0.0.0-host";
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
    fprintf(stderr, "bb_system_restart: host stub — exiting\n");
    exit(0);
}

#ifdef BB_SYSTEM_TESTING
static float    s_test_temp = 0.0f;
static bb_err_t s_test_rc   = BB_ERR_UNSUPPORTED;

void bb_system_set_temp_for_test(float celsius, bb_err_t rc)
{
    s_test_temp = celsius;
    s_test_rc   = rc;
}

bb_err_t bb_system_read_temp_celsius(float *out)
{
    if (!out) return BB_ERR_INVALID_ARG;
    if (s_test_rc == BB_OK) *out = s_test_temp;
    return s_test_rc;
}
#else
bb_err_t bb_system_read_temp_celsius(float *out)
{
    (void)out;
    return BB_ERR_UNSUPPORTED;
}
#endif
