// Portable core for bb_wifi_ap: no platform headers, compiled on host and
// ESP-IDF. Pure SSID derivation and captive-DNS packet building lifted out
// of bb_prov's espidf-only bb_prov_start_ap()/dns_task() (KB 781).
#include "bb_wifi_ap.h"
#include "bb_str.h"

#include <stdio.h>
#include <string.h>

bb_err_t bb_wifi_ap_build_ssid(const char *prefix, const uint8_t mac[6],
                                char *out, size_t out_size)
{
    if (!prefix || !mac || !out || out_size == 0) return BB_ERR_INVALID_ARG;
    // snprintf cannot return negative for this fixed, all-ASCII format
    // string ("%s%02X%02X"), so the only failure mode to guard is
    // truncation (the formatted SSID not fitting in out_size).
    size_t n = (size_t)snprintf(out, out_size, "%s%02X%02X", prefix, mac[4], mac[5]);
    if (n >= out_size) return BB_ERR_INVALID_ARG;
    return BB_OK;
}

void bb_wifi_ap_normalize_prefix(const char *prefix, char *out, size_t out_size)
{
    if (!out || out_size == 0) return;
    if (!prefix) {
        out[0] = '\0';
        return;
    }
    bb_strlcpy(out, prefix, out_size);
}

void bb_wifi_ap_normalize_password(const char *password, char *out, size_t out_size)
{
    if (!out || out_size == 0) return;
    bb_strlcpy(out, password ? password : "breadboard", out_size);
}

// DNS answer trailer appended after the (copied) question section: name
// pointer (0xC0 0x0C) + TYPE A + CLASS IN + TTL 60s + RDLENGTH 4 + IPv4.
#define BB_WIFI_AP_DNS_ANSWER_BYTES 16

int bb_wifi_ap_dns_build_response(const uint8_t *req, int req_len,
                                   const uint8_t answer_ip[4],
                                   uint8_t *out, int out_cap)
{
    if (!req || !answer_ip || !out) return 0;
    if (req_len < 12 || req_len > 512) return 0;  // out of the accepted DNS query size range

    int resp_len = req_len + BB_WIFI_AP_DNS_ANSWER_BYTES;
    if (resp_len > out_cap) return 0;

    memcpy(out, req, (size_t)req_len);
    out[2] = 0x81;  // QR=1 (response)
    out[3] = 0x80;  // AA=1 (authoritative), no error
    out[6] = 0;     // ANCOUNT high byte
    out[7] = 1;     // ANCOUNT = 1 answer

    int pos = req_len;
    out[pos++] = 0xC0;  // pointer to question name
    out[pos++] = 0x0C;
    out[pos++] = 0x00;  // type A
    out[pos++] = 0x01;
    out[pos++] = 0x00;  // class IN
    out[pos++] = 0x01;
    out[pos++] = 0x00;  // TTL 60 seconds
    out[pos++] = 0x00;
    out[pos++] = 0x00;
    out[pos++] = 0x3C;
    out[pos++] = 0x00;  // rdlength 4
    out[pos++] = 0x04;
    out[pos++] = answer_ip[0];
    out[pos++] = answer_ip[1];
    out[pos++] = answer_ip[2];
    out[pos++] = answer_ip[3];

    return pos;
}
