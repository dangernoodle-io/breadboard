#include "bb_health.h"

#include <stdbool.h>
#include <string.h>

#include "bb_board.h"
#include "bb_http.h"
#include "bb_http_server.h"
#include "bb_log.h"
#include "bb_mdns.h"
#include "bb_wifi.h"
#include "bb_wifi_http.h"

#include "../../../components/bb_health/bb_health_compose_priv.h"
#include "../../../components/bb_health/bb_health_schema_priv.h"
#include "../../../components/bb_health/bb_health_stack.h"
#include "bb_health_section.h"

// Forward declaration from bb_health_stack.c
bb_err_t bb_health_stack_monitor_init(void);

static const char *TAG = "bb_health";

// ---------------------------------------------------------------------------
// Route handler -- GATHER-THEN-STREAM (B1-1100)
// ---------------------------------------------------------------------------

// Gathers the ROOT wire slice (ok/validated/network) from device-specific
// sources (bb_wifi/bb_mdns/bb_board -- none of which are host-reproducible,
// which is why this gather stays in this ESP-IDF-only file) and hands it to
// bb_health_compose_and_stream() (bb_health_compose_priv.h), the portable
// seam that walks the bb_health_section registry and streams the composed
// document. This handler is a thin wrapper; the gather-then-stream
// algorithm itself is host-testable (test/test_host/test_bb_health_compose.c).
static bb_err_t health_handler(bb_http_request_t *req)
{
    bb_health_wire_t root;
    memset(&root, 0, sizeof(root));

    root.ok = bb_health_compute_ok();

    bb_board_info_t b;
    bb_board_get_info(&b);
    root.validated = b.ota_validated;

    // network: status bools/strings only (TA-505) -- ssid/bssid/ip/connected,
    // no numeric fields.
    bb_wifi_info_t info;
    bb_wifi_get_info(&info);
    strncpy(root.network.ssid, info.ssid, sizeof(root.network.ssid) - 1);
    root.network.ssid[sizeof(root.network.ssid) - 1] = '\0';
    bb_wifi_http_format_bssid(root.network.bssid, info.bssid);
    strncpy(root.network.ip, info.ip, sizeof(root.network.ip) - 1);
    root.network.ip[sizeof(root.network.ip) - 1] = '\0';
    root.network.connected = info.connected;

    const char *hostname = bb_mdns_get_hostname();
    root.network.mdns = hostname
        ? (bb_serialize_str_n_t){ .ptr = hostname, .len = strlen(hostname) }
        : (bb_serialize_str_n_t){ 0 };

    return bb_health_compose_and_stream(req, &root);
}

// ---------------------------------------------------------------------------
// Route descriptor
// ---------------------------------------------------------------------------

static bb_route_response_t s_health_responses[] = {
    { 200, "application/json",
      NULL,  // filled by bb_health_assemble_schema() at init
      "liveness check" },
    { 0 },
};

static const bb_route_t s_health_route = {
    .method    = BB_HTTP_GET,
    .path      = "/api/health",
    .tag       = "health",
    .summary   = "Get liveness status",
    .responses = s_health_responses,
    .handler   = health_handler,
};

bb_err_t bb_health_init(bb_http_handle_t server)
{
    if (!server) return BB_ERR_INVALID_ARG;

    // FREEZE TRIPWIRE (B1-1100): the bb_health_section registry is the
    // ONLY registry the live handler renders from now -- freeze it here,
    // at server-start, the same lifecycle point bb_health_init() used to
    // freeze the retired legacy bb_response registry.
    bb_health_section_freeze();

    // Assemble the schema exactly ONCE: bb_health_init() is re-entrant
    // (examples/floor's http_lifecycle_observer re-fires it on every WiFi
    // pause/resume) but the schema is static after the registry is frozen
    // above (frozen == no more section registrations, and freeze itself is
    // idempotent) -- reassembling on every re-entry would leak the previous
    // heap-allocated string (bb_health_assemble_schema() mallocs) for no
    // behavior change, since the bytes it produces can't differ.
    if (!s_health_responses[0].schema) {
        s_health_responses[0].schema = bb_health_assemble_schema();
    }

    bb_err_t err = bb_http_register_described_route(server, &s_health_route);
    if (err != BB_OK) return err;

    // Start the stack high-water monitor (no-op if FREERTOS_USE_TRACE_FACILITY=n).
    bb_err_t stack_err = bb_health_stack_monitor_init();
    if (stack_err != BB_OK) {
        bb_log_w(TAG, "stack monitor init failed: %d", (int)stack_err);
    }

    bb_log_i(TAG, "health route registered");
    return BB_OK;
}

// PRE_HTTP companion: declare route count before server starts.
bb_err_t bb_health_reserve_routes(void)
{
    bb_http_reserve_routes(1);  // GET /api/health
    return BB_OK;
}
