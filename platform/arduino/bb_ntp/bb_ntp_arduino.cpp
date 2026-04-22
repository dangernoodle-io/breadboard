#include <Arduino.h>
#include "bb_ntp.h"
#include "bb_log.h"

static const char *TAG = "bb_ntp";
static bool s_warned = false;

extern "C" bb_err_t bb_ntp_start(const char *server)
{
    if (!s_warned) {
        bb_log_w(TAG, "NTP not implemented on Arduino");
        s_warned = true;
    }
    return BB_OK;
}

extern "C" bb_err_t bb_ntp_stop(void)
{
    return BB_OK;
}

extern "C" bool bb_ntp_is_synced(void)
{
    return false;
}
