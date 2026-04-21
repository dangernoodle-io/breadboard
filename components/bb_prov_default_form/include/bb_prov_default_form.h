#pragma once

#include "bb_http.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Return a pointer to the pre-built asset for the default WiFi provisioning form.
 * path="/", mime="text/html", encoding="gzip".
 *
 * Pass to bb_prov_start() for bare-minimum bringup:
 *   const bb_http_asset_t *a = bb_prov_default_form_get();
 *   bb_prov_start(a, 1);
 */
const bb_http_asset_t *bb_prov_default_form_get(void);

#ifdef __cplusplus
}
#endif
