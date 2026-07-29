#include "bb_wifi_http_route_dup_status.h"

bb_err_t bb_wifi_http_route_register_outcome(bb_err_t rc)
{
    if (rc == BB_ERR_INVALID_STATE) return BB_OK;
    return rc;
}
