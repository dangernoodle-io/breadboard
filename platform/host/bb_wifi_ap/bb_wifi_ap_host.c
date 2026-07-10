// Host stub for bb_wifi_ap's ESP-IDF-only SoftAP lifecycle entry points.
// Never compiled into the espidf backend (that build gets
// platform/espidf/bb_wifi_ap/bb_wifi_ap.c instead via bb_wifi_ap's
// CMakeLists.txt) -- the #ifndef ESP_PLATFORM guard here is defense in
// depth, mirroring platform/host/bb_wifi/bb_wifi_host.c.
#include "bb_wifi_ap.h"
#include "bb_str.h"

#ifndef ESP_PLATFORM

static char s_ap_ssid[32] = "";

bb_err_t bb_wifi_ap_start(void)
{
    return BB_OK;
}

void bb_wifi_ap_stop(void)
{
}

void bb_wifi_ap_get_ssid(char *buf, size_t len)
{
    bb_strlcpy(buf, s_ap_ssid, len);
}

void bb_wifi_ap_set_ssid_prefix(const char *prefix)
{
    (void)prefix;
}

void bb_wifi_ap_set_password(const char *password)
{
    (void)password;
}

#endif /* !ESP_PLATFORM */
