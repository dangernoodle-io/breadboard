#include "wifi_prov_gate_wire.h"
#include "bb_http_prov_gate.h"

// The one place this file's "attempt every entry, keep the FIRST failure"
// decision lives -- called once per allowlist entry below rather than
// hand-rolled per call site (this repo's second-instance-triggers-
// extraction convention, wiki Conventions#reuse-or-extract). *first_err is
// only ever written once, by whichever call first fails; every later call
// (success or failure) leaves it alone.
static void gate_wire_try(bb_err_t *first_err, bb_http_method_t method, const char *path)
{
    bb_err_t err = bb_http_prov_allow(method, path);
    if (err != BB_OK && *first_err == BB_OK) {
        *first_err = err;
    }
}

// See wifi_prov_gate_wire_last_registration_complete()'s doc (header) --
// true (vacuously) until the first wifi_prov_gate_wire_register() call.
static bool s_last_registration_complete = true;

bb_err_t wifi_prov_gate_wire_register(const bb_http_asset_t *assets, size_t n)
{
    // Fixed entries first -- see wifi_prov_gate_wire.h for why these must
    // win BB_HTTP_PROV_ALLOWLIST_CAP over the per-asset loop below.
    bb_err_t first_err = BB_OK;

    gate_wire_try(&first_err, BB_HTTP_GET, "/ping");
    gate_wire_try(&first_err, BB_HTTP_POST, "/save");
    gate_wire_try(&first_err, BB_HTTP_POST, "/api/wifi/scan");

    for (size_t i = 0; i < n; i++) {
        gate_wire_try(&first_err, BB_HTTP_GET, assets[i].path);
    }

    s_last_registration_complete = (first_err == BB_OK);

    return first_err;
}

bool wifi_prov_gate_wire_last_registration_complete(void)
{
    return s_last_registration_complete;
}

void wifi_prov_gate_wire_reset_for_test(void)
{
    s_last_registration_complete = true;
}
