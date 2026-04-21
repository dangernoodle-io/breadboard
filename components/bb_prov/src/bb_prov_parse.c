#include "bb_prov.h"
#include "bb_http.h"

bb_prov_parse_result_t bb_prov_parse_body(
    const char *body, int body_len,
    char *ssid_out, size_t ssid_size,
    char *pass_out, size_t pass_size)
{
    if (body_len <= 0) {
        return BB_PROV_PARSE_EMPTY_BODY;
    }
    ssid_out[0] = '\0';
    pass_out[0] = '\0';
    bb_url_decode_field(body, "ssid", ssid_out, ssid_size);
    bb_url_decode_field(body, "pass", pass_out, pass_size);
    if (ssid_out[0] == '\0') {
        return BB_PROV_PARSE_SSID_REQUIRED;
    }
    return BB_PROV_PARSE_OK;
}
