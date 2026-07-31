#include "bb_wifi_http_patch_scratch.h"

#include "bb_http_serialize_error.h"

bb_err_t bb_wifi_http_patch_acquire_scratch(bb_http_request_t *req, bb_data_scratch_t *out)
{
    bb_err_t rc = bb_data_scratch_acquire(out);
    if (rc != BB_OK) {
        bb_http_serialize_send_error(req, 500, "internal error");
    }
    return rc;
}
