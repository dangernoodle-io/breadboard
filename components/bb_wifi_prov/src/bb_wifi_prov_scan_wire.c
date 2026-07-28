// bb_wifi_prov_scan_wire — wire descriptor + pure fill for bb_wifi_prov's
// own POST /api/wifi/scan response. See bb_wifi_prov_scan_wire.h for the
// {"aps":[...],"state":...} shape this produces and the state-machine
// rationale.

#include "bb_wifi_prov_scan_wire.h"

#include <stddef.h>
#include <string.h>

const bb_serialize_field_t bb_wifi_prov_scan_ap_wire_fields[3] = {
    { .key = "ssid", .type = BB_TYPE_STR,
      .offset = offsetof(bb_wifi_prov_scan_ap_wire_t, ssid),
      .max_len = sizeof(((bb_wifi_prov_scan_ap_wire_t *)0)->ssid) },
    { .key = "rssi", .type = BB_TYPE_I64,
      .offset = offsetof(bb_wifi_prov_scan_ap_wire_t, rssi) },
    { .key = "secure", .type = BB_TYPE_BOOL,
      .offset = offsetof(bb_wifi_prov_scan_ap_wire_t, secure) },
};

const uint16_t bb_wifi_prov_scan_ap_wire_n_fields =
    sizeof(bb_wifi_prov_scan_ap_wire_fields) / sizeof(bb_wifi_prov_scan_ap_wire_fields[0]);

// bb_wifi_prov_scan_ap_wire_n_fields (above) is a `const uint16_t`, not a
// constant expression — can't initialize `.n_children` with it even in this
// same TU (mirrors bb_wifi_http_scan_wire.c's identical BB_WIFI_HTTP_SCAN_
// AP_ROW_N_FIELDS precedent). test_wifi_prov_scan_wire_row_field_count_
// matches asserts this literal against the SAME extern the runtime count
// comes from.
#define BB_WIFI_PROV_SCAN_AP_ROW_N_FIELDS 3

static const bb_serialize_field_t s_scan_wire_fields[2] = {
    { .key = "aps", .type = BB_TYPE_ARR,
      .offset = offsetof(bb_wifi_prov_scan_wire_t, aps),
      .elem_type = BB_TYPE_OBJ, .elem_size = sizeof(bb_wifi_prov_scan_ap_wire_t),
      .max_items = WIFI_SCAN_MAX,
      .children = bb_wifi_prov_scan_ap_wire_fields,
      .n_children = BB_WIFI_PROV_SCAN_AP_ROW_N_FIELDS },
    { .key = "state", .type = BB_TYPE_STR,
      .offset = offsetof(bb_wifi_prov_scan_wire_t, state),
      .max_len = sizeof(((bb_wifi_prov_scan_wire_t *)0)->state) },
};

const bb_serialize_desc_t bb_wifi_prov_scan_wire_desc = {
    .type_name = "bb_wifi_prov_scan_wire_t",
    .fields    = s_scan_wire_fields,
    .n_fields  = sizeof(s_scan_wire_fields) / sizeof(s_scan_wire_fields[0]),
    .snap_size = sizeof(bb_wifi_prov_scan_wire_t),
};

bb_wifi_prov_scan_state_t bb_wifi_prov_scan_classify_state(bool ever_requested, bool idle)
{
    if (!ever_requested) {
        return BB_WIFI_PROV_SCAN_NOT_STARTED;
    }
    if (!idle) {
        return BB_WIFI_PROV_SCAN_PENDING;
    }
    return BB_WIFI_PROV_SCAN_READY;
}

const char *bb_wifi_prov_scan_state_str(bb_wifi_prov_scan_state_t state)
{
    if (state == BB_WIFI_PROV_SCAN_PENDING) {
        return "pending";
    }
    if (state == BB_WIFI_PROV_SCAN_READY) {
        return "ready";
    }
    return "not_started";
}

void bb_wifi_prov_scan_wire_copy_rows(bb_wifi_prov_scan_ap_wire_t *dst,
                                       const bb_wifi_ap_t *src, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        strncpy(dst[i].ssid, src[i].ssid, sizeof(dst[i].ssid) - 1);
        dst[i].ssid[sizeof(dst[i].ssid) - 1] = '\0';
        dst[i].rssi = (int64_t)src[i].rssi;
        dst[i].secure = src[i].secure;
    }
}

size_t bb_wifi_prov_scan_wire_clamp_count(int count)
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

void bb_wifi_prov_scan_wire_fill(bb_wifi_prov_scan_wire_t *dst,
                                  const bb_wifi_ap_t *aps, int count,
                                  bb_wifi_prov_scan_state_t state)
{
    memset(dst, 0, sizeof(*dst));

    size_t n = bb_wifi_prov_scan_wire_clamp_count(count);
    bb_wifi_prov_scan_wire_copy_rows(dst->aps_items, aps, n);
    dst->aps.items = dst->aps_items;
    dst->aps.count = n;

    strncpy(dst->state, bb_wifi_prov_scan_state_str(state), sizeof(dst->state) - 1);
    dst->state[sizeof(dst->state) - 1] = '\0';
}
