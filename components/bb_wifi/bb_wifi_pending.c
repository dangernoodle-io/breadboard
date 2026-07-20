#include "bb_wifi_pending.h"

#include <string.h>

bb_wifi_pending_action_t bb_wifi_pending_decide(uint8_t try_flag,
                                                const char *pending_ssid)
{
    if (try_flag == 0) {
        return BB_WIFI_PENDING_NONE;
    }
    if (pending_ssid == NULL || pending_ssid[0] == '\0') {
        return BB_WIFI_PENDING_NONE;
    }
    return BB_WIFI_PENDING_TRY;
}

bb_err_t bb_wifi_pending_validate(const char *ssid, const char *pass)
{
    if (ssid == NULL || ssid[0] == '\0') {
        return BB_ERR_INVALID_ARG;
    }
    if (strlen(ssid) > BB_WIFI_PENDING_SSID_MAX) {
        return BB_ERR_INVALID_ARG;
    }
    /* NULL pass is treated as empty string — open network is allowed. */
    if (pass != NULL && strlen(pass) > BB_WIFI_PENDING_PASS_MAX) {
        return BB_ERR_INVALID_ARG;
    }
    return BB_OK;
}

bb_err_t bb_wifi_pending_validate_buf(const char *ssid, size_t ssid_cap,
                                       const char *pass, size_t pass_cap)
{
    if (ssid == NULL) {
        return BB_ERR_INVALID_ARG;
    }

    size_t ssid_len = strnlen(ssid, ssid_cap);
    if (ssid_len == 0 || ssid_len > BB_WIFI_PENDING_SSID_MAX) {
        return BB_ERR_INVALID_ARG;
    }

    if (pass != NULL) {
        size_t pass_len = strnlen(pass, pass_cap);
        if (pass_len > BB_WIFI_PENDING_PASS_MAX) {
            return BB_ERR_INVALID_ARG;
        }
    }

    /* Both buffers are now proven NUL-terminated within bounds well under
     * their real limits, so strlen() below is safe. */
    return bb_wifi_pending_validate(ssid, pass);
}
