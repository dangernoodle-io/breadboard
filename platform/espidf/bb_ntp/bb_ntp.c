#include "bb_ntp.h"
#include "bb_log.h"
#include "bb_settings.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

static const char *TAG = "bb_ntp";
static const char *DEFAULT_SERVER = "pool.ntp.org";

static bool s_synced = false;
static bool s_started = false;
static int64_t s_last_sync_unix = 0;

static void ntp_time_sync_notification_cb(struct timeval *tv)
{
    bb_log_i(TAG, "SNTP time synchronized");
    s_last_sync_unix = (int64_t)tv->tv_sec;
    s_synced = true;
}

bb_err_t bb_ntp_start(const char *server)
{
    if (s_started) {
        return BB_OK;
    }

    const char *ntp_server = server ? server : DEFAULT_SERVER;

    bb_ntp_apply_saved_timezone();

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

int64_t bb_ntp_last_sync_unix(void)
{
    return s_last_sync_unix;
}

#define BB_NTP_SANE_EPOCH 1700000000  /* ~2023-11-14; clock below this is bogus */

bool bb_ntp_wait_synced(uint32_t timeout_ms)
{
    const TickType_t step = pdMS_TO_TICKS(250);
    uint32_t waited = 0;
    for (;;) {
        if (s_synced && (long)time(NULL) > BB_NTP_SANE_EPOCH) {
            return true;
        }
        if (waited >= timeout_ms) {
            return false;
        }
        vTaskDelay(step);
        waited += 250;
    }
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
