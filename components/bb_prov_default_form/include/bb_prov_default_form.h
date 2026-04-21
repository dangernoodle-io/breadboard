#pragma once

#include "bb_http.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Pre-built asset for the default WiFi provisioning form.
 * path="/", mime="text/html", encoding=NULL.
 *
 * Pass to bb_prov_start() for bare-minimum bringup:
 *   bb_prov_start(&bb_prov_default_form_asset, 1);
 */
extern bb_http_asset_t bb_prov_default_form_asset;

#ifdef __cplusplus
}
#endif
