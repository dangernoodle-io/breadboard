#pragma once

#include "bb_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Return a pointer to the pre-built asset for the default WiFi provisioning form.
 * path="/", mime="text/html", encoding="gzip".
 *
 * Pass to bb_wifi_prov_start() for bare-minimum bringup:
 *   const bb_http_asset_t *a = bb_wifi_prov_default_form_get();
 *   bb_wifi_prov_start(a, 1, NULL);
 *
 * Lives in its own translation unit so a consumer that never calls this
 * getter never pulls the generated gz blob into its image (static-archive
 * link semantics -- see bb_wifi_prov's CMakeLists.txt).
 */
const bb_http_asset_t *bb_wifi_prov_default_form_get(void);

#ifdef __cplusplus
}
#endif
