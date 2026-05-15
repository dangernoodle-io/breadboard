// Arduino stub for bb_http_client — returns BB_ERR_UNSUPPORTED. Arduino
// targets in this workspace are AVR / Cortex-M class with no general HTTPS
// client. Implement per-board if a real consumer appears.
#include "bb_http_client.h"

bb_err_t bb_http_client_get(const char *url,
                            char *body, size_t body_cap,
                            const bb_http_client_cfg_t *cfg,
                            bb_http_client_result_t *out)
{
    (void)url; (void)body; (void)body_cap; (void)cfg;
    if (out) {
        out->status_code = 0;
        out->body_len = 0;
        out->truncated = false;
    }
    return BB_ERR_UNSUPPORTED;
}
