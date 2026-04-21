#include "bb_prov_default_form.h"
#include <stdint.h>

extern const uint8_t prov_default_form_html_start[] asm("_binary_prov_default_form_html_start");
extern const uint8_t prov_default_form_html_end[]   asm("_binary_prov_default_form_html_end");

/* Defined without const so len can be set by the constructor.
   The public header declares it extern const — callers get read-only access. */
bb_http_asset_t bb_prov_default_form_asset = {
    .path     = "/",
    .mime     = "text/html",
    .encoding = NULL,
    .data     = NULL,
    .len      = 0,
};

__attribute__((constructor))
static void bb_prov_default_form_init(void)
{
    bb_prov_default_form_asset.data = prov_default_form_html_start;
    bb_prov_default_form_asset.len  =
        (size_t)(prov_default_form_html_end - prov_default_form_html_start);
}
