#include "bb_ntp.h"
#include "bb_log.h"

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
