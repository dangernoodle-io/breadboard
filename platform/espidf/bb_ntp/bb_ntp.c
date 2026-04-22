#include "bb_ntp.h"
#include "bb_log.h"
#include "esp_sntp.h"
#include <string.h>
#include <sys/time.h>

static const char *TAG = "bb_ntp";
static const char *DEFAULT_SERVER = "pool.ntp.org";

static bool s_synced = false;
static bool s_started = false;

static void ntp_time_sync_notification_cb(struct timeval *tv)
{
    bb_log_i(TAG, "SNTP time synchronized");
    s_synced = true;
}

bb_err_t bb_ntp_start(const char *server)
{
    if (s_started) {
        return BB_OK;
    }

    const char *ntp_server = server ? server : DEFAULT_SERVER;

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, (char *)ntp_server);
    esp_sntp_set_time_sync_notification_cb(ntp_time_sync_notification_cb);
    esp_sntp_init();

    s_started = true;
    bb_log_i(TAG, "NTP client started with server: %s", ntp_server);

    return BB_OK;
}

bb_err_t bb_ntp_stop(void)
{
    if (!s_started) {
        return BB_OK;
    }

    esp_sntp_stop();
    s_started = false;
    bb_log_i(TAG, "NTP client stopped");

    return BB_OK;
}

bool bb_ntp_is_synced(void)
{
    return s_synced;
}
