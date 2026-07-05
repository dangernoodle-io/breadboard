#include "cache_route_status.h"

int cache_route_map_status(bb_err_t rc)
{
    if (rc == BB_OK) return 200;
    if (rc == BB_ERR_NOT_FOUND) return 404;
    if (rc == BB_ERR_INVALID_STATE) return 404;
    if (rc == BB_ERR_NO_SPACE) return 500;
    return 500;
}
