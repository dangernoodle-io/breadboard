#include "bb_prov_default_form.h"
#include <stddef.h>
#include <stdint.h>

extern const uint8_t bb_prov_default_form_gz[];
extern const size_t  bb_prov_default_form_gz_len;

const bb_http_asset_t *bb_prov_default_form_get(void)
{
    static bb_http_asset_t s_asset;
    static int s_init = 0;
    if (!s_init) {
        s_asset.path     = "/";
        s_asset.mime     = "text/html";
        s_asset.encoding = "gzip";
        s_asset.data     = bb_prov_default_form_gz;
        s_asset.len      = bb_prov_default_form_gz_len;
        s_init = 1;
    }
    return &s_asset;
}
