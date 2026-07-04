#include "sse_connect_error_decision.h"

int sse_connect_error_status(bb_err_t err)
{
    if (err == BB_ERR_NO_SPACE) return 503;
    return 500;
}
