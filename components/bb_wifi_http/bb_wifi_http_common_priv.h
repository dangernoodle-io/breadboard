#pragma once

// bb_wifi_http_common — shared bb_wifi_info_t -> fields fill helper
// (B1-1106). bb_wifi_http_info_wire_fill (bb_wifi_http_wire.c) and
// bb_wifi_http_diag_fill (bb_wifi_http_diag.c) both need the IDENTICAL
// ssid/bssid/rssi/ip/connected/disc_reason/disc_age_s/retry_count/
// restart_sta_count/disconnect_rssi copy out of a bb_wifi_info_t plus the
// two global recovery-telemetry getters (bb_wifi_get_restart_sta_count /
// bb_wifi_get_disconnect_rssi) -- this was hand-rolled twice (the second
// instance triggers extraction per the consolidation rule). Pure/portable:
// no ESP-IDF/FreeRTOS types, compiles on host + ESP-IDF.
//
// Out-parameter style (rather than a shared struct type) deliberately keeps
// bb_wifi_http_info_wire_t and bb_wifi_http_diag_snap_t's public layouts
// untouched -- each caller passes pointers straight at its own struct's
// members, so no new struct-layout assumption is introduced and no
// existing public header/type changes.

#include "bb_serialize.h"
#include "bb_wifi.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Fills the shared fields from `info` plus the two global
// recovery-telemetry getters. `ssid`/`bssid`/`ip` are fixed-size output
// buffers; callers pass `sizeof(dst->ssid)`/`sizeof(dst->bssid)`/
// `sizeof(dst->ip)` for `ssid_sz`/`bssid_sz`/`ip_sz` (the
// bb_strlcpy(dst, src, sizeof(dst)) idiom) rather than hardcoded literals.
// Every other out-param is a single scalar/struct pointer. None of the
// pointers may be NULL.
void bb_wifi_http_fill_common(char *ssid, size_t ssid_sz, char *bssid,
                               size_t bssid_sz, int64_t *rssi, char *ip,
                               size_t ip_sz, bool *connected,
                               bb_serialize_str_n_t *disc_reason,
                               int64_t *disc_age_s, int64_t *retry_count,
                               int64_t *restart_sta_count,
                               int64_t *disconnect_rssi,
                               const bb_wifi_info_t *info);

#ifdef __cplusplus
}
#endif
