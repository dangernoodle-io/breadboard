// bb_wifi_http_common -- see bb_wifi_http_common_priv.h for the fill
// contract (B1-1106).

#include "bb_wifi_http_common_priv.h"

#include "bb_wifi_http.h"

#include <string.h>

void bb_wifi_http_fill_common(char *ssid, size_t ssid_sz, char *bssid,
                               size_t bssid_sz, int64_t *rssi, char *ip,
                               size_t ip_sz, bool *connected,
                               bb_serialize_str_n_t *disc_reason,
                               int64_t *disc_age_s, int64_t *retry_count,
                               int64_t *restart_sta_count,
                               int64_t *disconnect_rssi,
                               const bb_wifi_info_t *info)
{
    // bssid_sz: bb_wifi_http_format_bssid() always writes a fixed
    // 18-byte "aa:bb:cc:dd:ee:ff" label into `bssid` -- both callers'
    // bssid buffers are exactly that size, so there is no truncation to
    // parameterize here. Kept for signature symmetry with ssid_sz/ip_sz.
    (void)bssid_sz;

    strncpy(ssid, info->ssid, ssid_sz - 1);
    ssid[ssid_sz - 1] = '\0';
    bb_wifi_http_format_bssid(bssid, info->bssid);
    *rssi = (int64_t)info->rssi;
    strncpy(ip, info->ip, ip_sz - 1);
    ip[ip_sz - 1] = '\0';
    *connected = info->connected;

    const char *reason_str = bb_wifi_disc_reason_str(info->disc_reason);
    *disc_reason = (bb_serialize_str_n_t){ .ptr = reason_str, .len = strlen(reason_str) };

    *disc_age_s        = (int64_t)info->disc_age_s;
    *retry_count       = (int64_t)info->retry_count;
    *restart_sta_count = (int64_t)bb_wifi_get_restart_sta_count();
    *disconnect_rssi   = (int64_t)bb_wifi_get_disconnect_rssi();
}
