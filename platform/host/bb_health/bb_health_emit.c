// bb_health_emit — shared health ok computation.
// Compiled on both host (tests) and ESP-IDF (bb_health route).
// Single source of truth for the /api/health.ok gate.
#include "bb_health.h"
#include "bb_ota_validator.h"
#include "bb_wifi.h"

// Returns true when the device is operationally healthy:
//   - WiFi STA has obtained an IP address
//   - The running OTA slot is validated
//
// mDNS is intentionally excluded from the gate (locked decision B1-269):
// the network.mdns field is still emitted in /api/health.network, but
// mdns availability no longer blocks the ok flag.
bool bb_health_compute_ok(void)
{
    return bb_wifi_has_ip() && bb_ota_is_validated();
}
