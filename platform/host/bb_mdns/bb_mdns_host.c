#include "bb_nv.h"
#include "bb_mdns.h"
#include "bb_log.h"

static const char *TAG = "bb_mdns";

bb_err_t bb_mdns_browse_start(const char *service, const char *proto,
                              bb_mdns_peer_cb on_peer,
                              bb_mdns_peer_removed_cb on_removed,
                              void *ctx)
{
    if (!service || !proto) {
        return BB_ERR_INVALID_ARG;
    }
    bb_log_d(TAG, "browse_start stub: %s.%s", service, proto);
    return BB_OK;
}

bb_err_t bb_mdns_browse_stop(const char *service, const char *proto)
{
    if (!service || !proto) {
        return BB_ERR_INVALID_ARG;
    }
    bb_log_d(TAG, "browse_stop stub: %s.%s", service, proto);
    return BB_OK;
}
