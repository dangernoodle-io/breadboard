#include "bb_wifi_prov.h"
#include "bb_http_server.h"

bb_wifi_prov_parse_result_t bb_wifi_prov_parse_body(
    const char *body, int body_len,
    char *ssid_out, size_t ssid_size,
    char *pass_out, size_t pass_size,
    char *hostname_out, size_t hostname_size)
{
    if (body_len <= 0) {
        return BB_WIFI_PROV_PARSE_EMPTY_BODY;
    }
    ssid_out[0] = '\0';
    pass_out[0] = '\0';
    hostname_out[0] = '\0';
    bb_url_decode_field(body, "ssid", ssid_out, ssid_size);
    bb_url_decode_field(body, "pass", pass_out, pass_size);
    // Optional -- absent/empty is never an error (see this fn's header doc);
    // no charset/shape validation here, that's bb_settings_hostname_set()'s
    // job at the call site.
    bb_url_decode_field(body, "hostname", hostname_out, hostname_size);
    if (ssid_out[0] == '\0') {
        return BB_WIFI_PROV_PARSE_SSID_REQUIRED;
    }
    return BB_WIFI_PROV_PARSE_OK;
}
