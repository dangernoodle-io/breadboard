// bb_health_emit — shared health ok computation.
// Compiled on both host (tests) and ESP-IDF (bb_health route).
// Single source of truth for the /api/health.ok gate.
#include "bb_health.h"
#include "bb_wifi.h"

// Returns true when the device is operationally healthy: WiFi STA has
// obtained an IP address.
//
// ota_validated dropped (B1-977, bb_board dissolution): the gate previously
// ANDed in bb_board_get_info().ota_validated (lenient != PENDING_VERIFY);
// OTA re-contributes its own health section later (out of scope here) rather
// than reviving that term.
//
// mDNS is intentionally excluded from the gate (locked decision B1-269):
// the network.mdns field is still emitted in /api/health.network, but
// mdns availability no longer blocks the ok flag.
bool bb_health_compute_ok(void)
{
    return bb_wifi_has_ip();
}
