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
 * OPT-IN, by a single call at the consumer's composition root -- this is
 * not auto-registered, and bb_wifi_prov never calls it internally. A
 * consumer that supplies its own form simply never calls this getter and
 * pays zero bytes for it: the gz blob lives in its own translation unit
 * (bb_wifi_prov_default_form.c), so on ESP-IDF's static-archive link
 * semantics the linker only pulls that object file in when something
 * resolves the undefined symbol against it. Measured on origin/main
 * before the B1-1255 fold-in (commit 0e09358e, PR #1124), on
 * examples/floor's ESP32 ELF: calling this getter costs 1216 bytes of
 * firmware.bin (the 1117-byte gzipped form asset plus its wrapper);
 * omitting the call, every related symbol is absent from the image.
 */
const bb_http_asset_t *bb_wifi_prov_default_form_get(void);

#ifdef __cplusplus
}
#endif
