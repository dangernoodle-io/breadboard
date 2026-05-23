#include "bb_http.h"
#include "bb_registry.h"

// GET /api/board was removed — superseded by GET /api/info which includes
// the same board fields plus heap and network info. No routes registered here.

static bb_err_t bb_board_init(bb_http_handle_t server)
{
    if (!server) return BB_ERR_INVALID_ARG;
    return BB_OK;
}

#if CONFIG_BB_BOARD_AUTOREGISTER
BB_REGISTRY_REGISTER_N(bb_board, bb_board_init, 1);
#endif
