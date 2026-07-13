#include "bb_system.h"

#include "bb_log.h"

#include <inttypes.h>
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
#define X(v, s) case v: return s;
        BB_RESET_REASON_LIST(X)
#undef X
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

void bb_system_restart_reason(bb_reset_source_t src, const char *detail)
{
    bb_system_restart_reason_at(src, detail, 0);
}

void bb_system_restart_reason_at(bb_reset_source_t src, const char *detail, uint32_t caller_epoch_s)
{
    fprintf(stderr, "bb_system_restart_reason_at: arduino stub — src=%s detail=%s caller_epoch_s=%" PRIu32 " — exiting\n",
            bb_reset_source_str(src), detail ? detail : "", caller_epoch_s);
    exit(0);
}

// Boot-health counter (B1-753) — no Arduino consumer today (bb_wifi/
// bb_ota_validator's boot-count call sites are ESP-IDF only); in-memory
// stand-in kept for link-surface completeness, matching the host stub.
static uint8_t s_boot_count = 0;

bb_err_t bb_system_boot_count_increment(void)
{
    if (s_boot_count < UINT8_MAX) s_boot_count++;
    return BB_OK;
}

bb_err_t bb_system_boot_count_reset(void)
{
    s_boot_count = 0;
    return BB_OK;
}
