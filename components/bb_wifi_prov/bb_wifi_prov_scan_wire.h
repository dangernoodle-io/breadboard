#pragma once

// bb_wifi_prov_scan_wire — private wire descriptor + pure state classifier
// for bb_wifi_prov's own POST /api/wifi/scan response (B1-809 portal). This
// is a SEPARATE descriptor from bb_wifi_http's
// (components/bb_wifi_http/bb_wifi_http_scan_wire_priv.h) — bb_wifi_prov
// must not REQUIRE bb_wifi_http (it must OWN this endpoint, not depend on
// the other component to serve it) — but the "aps" row shape is kept
// byte-identical ({"ssid":string,"rssi":int,"secure":bool}) so the
// provisioning form's JS is portable between whichever component actually
// answers the request (see route_registry.c's first-registration-wins
// coexistence rule, exercised in bb_wifi_prov_start()).
//
// Extended with one field bb_wifi_http's shape doesn't have: "state", a
// short string distinguishing why "aps" may be empty:
//   "not_started" — no scan has ever been requested on this portal
//                   instance; "aps" is definitionally empty and tells the
//                   caller nothing about the RF environment.
//   "pending"     — a scan was requested (at portal start, or via this
//                   route's "refresh" query param) and is still in flight;
//                   "aps" reflects the last completed scan, if any, and may
//                   still be empty.
//   "ready"       — at least one scan has completed and none is currently
//                   in flight; "aps" is authoritative — an empty array here
//                   genuinely means no networks were found.
//
// CAUTION for callers using the "refresh" query param (bb_wifi_prov.c's
// prov_scan_handler): the rescan it triggers hops the radio channel, which
// transiently drops SoftAP-associated clients -- including whichever client
// just issued the "refresh" request. The response carrying this wire shape
// is written before that disruption in practice (the rescan runs as a
// detached low-priority task, not inline), but that ordering is scheduler-
// cooperative, not fence-guaranteed -- a portal UI should expect its own
// connection to drop and retry/reconnect rather than treat it as an error.
//
// Host-testable seam: bb_wifi_prov_scan_classify_state() takes plain bools
// (never calls bb_wifi_scan_wait_idle() itself, which is ESP-IDF-only — see
// bb_wifi.h) so both the ESP-IDF handler and host tests drive the SAME
// classification logic. bb_wifi_prov_scan_wire_fill() below is likewise
// pure: it takes an already-gathered AP array/count/state rather than
// calling bb_wifi_scan_start_async()/bb_wifi_scan_get_cached() itself — the
// side-effecting scan-trigger-and-read sequence lives in the ESP-IDF-only
// handler (platform/espidf/bb_wifi_prov/bb_wifi_prov.c), deliberately NOT
// mirroring bb_wifi_http_scan_wire_fill()'s defect (trigger-then-
// immediately-read-cache, which always returns an empty list on the first
// call — see that function's own doc comment).

#include "bb_serialize.h"
#include "bb_wifi.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// One access-point row — same 3 fields, same order, same wire types as
// bb_wifi_http_scan_ap_wire_t (components/bb_wifi_http/
// bb_wifi_http_scan_wire_priv.h) — kept in sync by inspection since the two
// components must not depend on each other.
typedef struct {
    char    ssid[33];
    int64_t rssi;
    bool    secure;
} bb_wifi_prov_scan_ap_wire_t;

extern const bb_serialize_field_t bb_wifi_prov_scan_ap_wire_fields[3];
extern const uint16_t             bb_wifi_prov_scan_ap_wire_n_fields;

// Scan-state classification (see module doc above).
typedef enum {
    BB_WIFI_PROV_SCAN_NOT_STARTED = 0,
    BB_WIFI_PROV_SCAN_PENDING,
    BB_WIFI_PROV_SCAN_READY,
} bb_wifi_prov_scan_state_t;

// Pure classifier: `ever_requested` is true once bb_wifi_prov has triggered
// at least one scan (the initial prefetch in bb_wifi_prov_start(), or a
// "refresh"-flagged POST /api/wifi/scan); `idle` is
// bb_wifi_scan_wait_idle(0)'s result (non-blocking with a 0 timeout — see
// that function's own doc comment). Both are caller-supplied plain bools —
// this function makes no ESP-IDF call itself, so it is fully host-testable
// independent of bb_wifi's scan machinery.
bb_wifi_prov_scan_state_t bb_wifi_prov_scan_classify_state(bool ever_requested, bool idle);

// Maps a state to its wire string ("not_started"/"pending"/"ready"). Any
// value other than BB_WIFI_PROV_SCAN_PENDING/_READY (including a future,
// out-of-range enumerator) defensively renders "not_started" rather than
// emitting an unrecognized string.
const char *bb_wifi_prov_scan_state_str(bb_wifi_prov_scan_state_t state);

// Top-level wire snapshot: "aps" (same array carrier/backing-storage split
// as bb_wifi_http_scan_wire_t) plus "state" (see module doc above).
typedef struct {
    bb_wifi_prov_scan_ap_wire_t aps_items[WIFI_SCAN_MAX];
    bb_serialize_arr_t          aps;
    char                        state[16];
} bb_wifi_prov_scan_wire_t;

extern const bb_serialize_desc_t bb_wifi_prov_scan_wire_desc;

// Pure row-copy helper — identical contract to
// bb_wifi_http_scan_wire_copy_rows(): NUL-safe (strncpy + explicit
// terminate), caller bounds `n` to WIFI_SCAN_MAX.
void bb_wifi_prov_scan_wire_copy_rows(bb_wifi_prov_scan_ap_wire_t *dst,
                                       const bb_wifi_ap_t *src, size_t n);

// Pure clamp helper — identical contract to
// bb_wifi_http_scan_wire_clamp_count(): bounds a raw
// bb_wifi_scan_get_cached() return into [0, WIFI_SCAN_MAX], guarding a
// negative return (which would otherwise wrap on the (size_t) cast) and an
// over-cap return.
size_t bb_wifi_prov_scan_wire_clamp_count(int count);

// Fills `dst` from an ALREADY-GATHERED scan snapshot: zero-inits `dst`,
// clamps+copies up to WIFI_SCAN_MAX rows from `aps`/`count`, wires
// `dst->aps` (items/count) to `dst->aps_items`, and renders `state` via
// bb_wifi_prov_scan_state_str(). Pure — see module doc above for why the
// scan-trigger/cache-read side effects live in the caller instead.
void bb_wifi_prov_scan_wire_fill(bb_wifi_prov_scan_wire_t *dst,
                                  const bb_wifi_ap_t *aps, int count,
                                  bb_wifi_prov_scan_state_t state);

#ifdef __cplusplus
}
#endif
