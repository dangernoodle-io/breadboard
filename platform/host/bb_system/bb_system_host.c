#include "bb_system.h"

#ifdef BB_SYSTEM_TESTING
#include "bb_system_test.h"
#endif

#include "bb_log.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "bb_system";

// Compile-time guard: BB_RESET_REASON_LIST must have exactly one X() entry per
// mapped bb_reset_reason_t value (every enumerator except the UNKNOWN
// default-case sentinel and the trailing COUNT sentinel). Catches drift
// between the enum and the X-macro at build time instead of silently falling
// through to "unknown" for a real reason.
#define BB_RESET_REASON_LIST_COUNT_ONE(v, s) +1
_Static_assert((0 BB_RESET_REASON_LIST(BB_RESET_REASON_LIST_COUNT_ONE)) == (BB_RESET_REASON_COUNT - 1),
               "BB_RESET_REASON_LIST entry count must match bb_reset_reason_t cardinality (excluding UNKNOWN)");
#undef BB_RESET_REASON_LIST_COUNT_ONE

bb_reset_reason_t bb_system_get_reset_reason(void)
{
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

void bb_system_restart_reason(bb_reset_source_t src, const char *detail)
{
    bb_system_restart_reason_at(src, detail, 0);
}

void bb_system_restart_reason_at(bb_reset_source_t src, const char *detail, uint32_t caller_epoch_s)
{
    fprintf(stderr, "bb_system_restart_reason_at: host stub — src=%s detail=%s caller_epoch_s=%" PRIu32 " — exiting\n",
            bb_reset_source_str(src), detail ? detail : "", caller_epoch_s);
    exit(0);
}

bb_err_t bb_system_get_app_sha256(char *out, size_t out_size)
{
    if (!out || out_size == 0) return BB_ERR_INVALID_ARG;
    // 9 hex chars + NUL
    if (out_size <= 9) return BB_ERR_NO_SPACE;
    // Fixed test value; matches the test expectation in test_bb_info_build.c
    const char *test_sha = "deadbeef0";
    size_t len = 9;
    for (size_t i = 0; i < len; i++) out[i] = test_sha[i];
    out[len] = '\0';
    return BB_OK;
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
