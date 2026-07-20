#include "bb_wifi_http_apply_status.h"

int bb_wifi_http_status_for_apply_rc(bb_err_t rc)
{
    if (rc == BB_ERR_VALIDATION || rc == BB_ERR_PARSE_GRAMMAR || rc == BB_ERR_PARSE_INCOMPLETE
        || rc == BB_ERR_INVALID_ARG || rc == BB_ERR_UNSUPPORTED) {
        return 400;
    }
    if (rc != BB_OK) return 500;
    return 202;
}
