// bb_wifi_creds_read -- pure provider-vs-fallback dispatch (no ESP-IDF, no
// bb_wifi state). Compiled on host and ESP-IDF. See bb_wifi_creds.h for the
// contract this closes: neither the provider nor the fallback is ever
// called with a NULL out_len, which is load-bearing for providers (e.g.
// bb_settings) that forward to bb_config_get_str -- that function rejects
// out_len==NULL and writes nothing to buf.

#include "bb_wifi_creds.h"

bb_err_t bb_wifi_creds_read(bb_wifi_creds_get_fn provider_fn, void *pctx,
                             bb_wifi_creds_get_fn fallback_fn, void *fctx,
                             char *buf, size_t cap, size_t *out_len)
{
    size_t len = 0;
    bb_err_t err;

    if (provider_fn) {
        err = provider_fn(pctx, buf, cap, &len);
    } else {
        err = fallback_fn(fctx, buf, cap, &len);
    }

    if (out_len) *out_len = len;
    return err;
}
