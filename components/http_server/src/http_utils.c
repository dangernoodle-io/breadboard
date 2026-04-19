#include "http_server.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// Extract a field from URL-encoded body: "field=value&..."
// Handles %XX decoding and + as space
void bsp_url_decode_field(const char *body, const char *field, char *out, size_t out_size)
{
    out[0] = '\0';
    // Find "field=" in body
    char key[64];
    snprintf(key, sizeof(key), "%s=", field);
    const char *start = strstr(body, key);
    if (!start) return;
    start += strlen(key);
    const char *end = strchr(start, '&');
    if (!end) end = start + strlen(start);

    size_t i = 0;
    while (start < end && i < out_size - 1) {
        if (*start == '+') {
            out[i++] = ' ';
            start++;
        } else if (*start == '%' && start + 2 < end) {
            char hex[3] = { start[1], start[2], '\0' };
            out[i++] = (char)strtoul(hex, NULL, 16);
            start += 3;
        } else {
            out[i++] = *start++;
        }
    }
    out[i] = '\0';
}

bsp_prov_parse_result_t bsp_prov_parse_body(
    const char *body, int body_len,
    char *ssid_out, size_t ssid_size,
    char *pass_out, size_t pass_size)
{
    if (body_len <= 0) {
        return BSP_PROV_PARSE_EMPTY_BODY;
    }
    // Caller guarantees body buffer has body_len+1 slots with a NUL already written,
    // matching how prov_save_handler uses it. bsp_url_decode_field expects C-string input.
    ssid_out[0] = '\0';
    pass_out[0] = '\0';
    bsp_url_decode_field(body, "ssid", ssid_out, ssid_size);
    bsp_url_decode_field(body, "pass", pass_out, pass_size);
    if (ssid_out[0] == '\0') {
        return BSP_PROV_PARSE_SSID_REQUIRED;
    }
    return BSP_PROV_PARSE_OK;
}
