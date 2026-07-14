#include "bb_ntp.h"
#include "bb_settings.h"
#include "bb_log.h"
#include <stdlib.h>
#include <time.h>

static const char *TAG = "bb_ntp";
static bool s_warned = false;

bb_err_t bb_ntp_start(const char *server)
{
    if (!s_warned) {
        bb_log_w(TAG, "NTP not implemented on host");
        s_warned = true;
    }
    return BB_OK;
}

bb_err_t bb_ntp_stop(void)
{
    return BB_OK;
}

bool bb_ntp_is_synced(void)
{
    return false;
}

bool bb_ntp_wait_synced(uint32_t timeout_ms)
{
    (void)timeout_ms;
    return false;
}

int64_t bb_ntp_last_sync_unix(void)
{
    return 0;
}

void bb_ntp_apply_saved_timezone(void)
{
    char tz[65] = {0};
    bb_err_t err = bb_settings_timezone_get(tz, sizeof(tz), NULL);
    if (err == BB_OK && tz[0] != '\0') {
        setenv("TZ", tz, 1);
    } else {
        setenv("TZ", "UTC0", 1);
    }
    tzset();
}

bb_err_t bb_ntp_set_timezone(const char *posix_tz)
{
    bb_err_t err = bb_settings_timezone_set(posix_tz);
    if (err != BB_OK) return err;
    const char *t = (posix_tz && posix_tz[0] != '\0') ? posix_tz : "UTC0";
    setenv("TZ", t, 1);
    tzset();
    return BB_OK;
}
