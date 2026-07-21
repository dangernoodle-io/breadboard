// bb_wifi_http_scan_wire — wire descriptor + fill for the POST
// /api/wifi/scan response. See bb_wifi_http_scan_wire_priv.h for the
// {"aps":[...]} shape this migration produces.

#include "bb_wifi_http_scan_wire_priv.h"

#include <stddef.h>
#include <string.h>

const bb_serialize_field_t bb_wifi_http_scan_ap_wire_fields[3] = {
    { .key = "ssid", .type = BB_TYPE_STR,
      .offset = offsetof(bb_wifi_http_scan_ap_wire_t, ssid),
      .max_len = sizeof(((bb_wifi_http_scan_ap_wire_t *)0)->ssid) },
    { .key = "rssi", .type = BB_TYPE_I64,
      .offset = offsetof(bb_wifi_http_scan_ap_wire_t, rssi) },
    { .key = "secure", .type = BB_TYPE_BOOL,
      .offset = offsetof(bb_wifi_http_scan_ap_wire_t, secure) },
};

const uint16_t bb_wifi_http_scan_ap_wire_n_fields =
    sizeof(bb_wifi_http_scan_ap_wire_fields) / sizeof(bb_wifi_http_scan_ap_wire_fields[0]);

// bb_wifi_http_scan_ap_wire_n_fields (below) is a `const uint16_t`, not a
// constant expression -- it can't initialize `.n_children` even in this
// same TU. The literal below encodes the documented 3-field row shape
// above; a drift is caught by test_wifi_scan_wire_row_field_count_matches
// asserting bb_wifi_http_scan_ap_wire_n_fields == 3 against the SAME extern
// the runtime count comes from (mirrors bb_diag_storage_partitions.c's
// BB_PARTITION_ROW_N_FIELDS precedent).
#define BB_WIFI_HTTP_SCAN_AP_ROW_N_FIELDS 3

static const bb_serialize_field_t s_scan_wire_fields[1] = {
    { .key = "aps", .type = BB_TYPE_ARR,
      .offset = offsetof(bb_wifi_http_scan_wire_t, aps),
      .elem_type = BB_TYPE_OBJ, .elem_size = sizeof(bb_wifi_http_scan_ap_wire_t),
      .max_items = WIFI_SCAN_MAX,
      .children = bb_wifi_http_scan_ap_wire_fields,
      .n_children = BB_WIFI_HTTP_SCAN_AP_ROW_N_FIELDS },
};

const bb_serialize_desc_t bb_wifi_http_scan_wire_desc = {
    .type_name = "bb_wifi_http_scan_wire_t",
    .fields    = s_scan_wire_fields,
    .n_fields  = sizeof(s_scan_wire_fields) / sizeof(s_scan_wire_fields[0]),
    .snap_size = sizeof(bb_wifi_http_scan_wire_t),
};

void bb_wifi_http_scan_wire_copy_rows(bb_wifi_http_scan_ap_wire_t *dst,
                                       const bb_wifi_ap_t *src, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        strncpy(dst[i].ssid, src[i].ssid, sizeof(dst[i].ssid) - 1);
        dst[i].ssid[sizeof(dst[i].ssid) - 1] = '\0';
        dst[i].rssi = (int64_t)src[i].rssi;
        dst[i].secure = src[i].secure;
    }
}

size_t bb_wifi_http_scan_wire_clamp_count(int count)
{
    if (count <= 0) {
        return 0;
    }

    size_t n = (size_t)count;
    if (n > (size_t)WIFI_SCAN_MAX) {
        n = WIFI_SCAN_MAX;
    }
    return n;
}

void bb_wifi_http_scan_wire_fill(bb_wifi_http_scan_wire_t *dst)
{
    memset(dst, 0, sizeof(*dst));

    bb_wifi_scan_start_async();

    bb_wifi_ap_t aps[WIFI_SCAN_MAX];
    memset(aps, 0, sizeof(aps));
    int count = bb_wifi_scan_get_cached(aps, WIFI_SCAN_MAX);
    size_t n = bb_wifi_http_scan_wire_clamp_count(count);

    bb_wifi_http_scan_wire_copy_rows(dst->aps_items, aps, n);
    dst->aps.items = dst->aps_items;
    dst->aps.count = n;
}
