// bb_health_emit — shared health ok computation.
// Compiled on both host (tests) and ESP-IDF (bb_health route).
// Single source of truth for the /api/health.ok gate.
#include "bb_health.h"
#include "bb_board.h"
#include "bb_wifi.h"

// Returns true when the device is operationally healthy:
//   - WiFi STA has obtained an IP address
//   - The running OTA slot is validated (lenient: true unless PENDING_VERIFY)
//
// Uses bb_board_get_info().ota_validated (lenient != PENDING_VERIFY) as the
// single source of truth — so health.ok and the reported validated state can
// never disagree.  bb_ota_is_validated() is
// intentionally NOT used here: its strict (== ESP_OTA_IMG_VALID) semantics
// serve the OTA rollback/retry logic (bb_wifi.c retry loop) and would
// cause health.ok=false on non-rollback / direct-flash / UNDEFINED builds
// even though the device is functioning correctly.
//
// mDNS is intentionally excluded from the gate (locked decision B1-269):
// the network.mdns field is still emitted in /api/health.network, but
// mdns availability no longer blocks the ok flag.
bool bb_health_compute_ok(void)
{
    bb_board_info_t info;
    if (bb_board_get_info(&info) != BB_OK) {
        return false;
    }
    return bb_wifi_has_ip() && info.ota_validated;
}
