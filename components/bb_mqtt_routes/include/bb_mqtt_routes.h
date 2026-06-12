// bb_mqtt_routes — GET + PATCH /api/mqtt HTTP routes.
//
// GET  /api/mqtt  → {uri, client_id, username, password:<masked>, tls,
//                    ca_set, cert_set, key_set, enabled, connected}
// PATCH /api/mqtt → parse JSON body, persist fields to NVS "bb_mqtt";
//                   returns 204 on success.
//
// Secrets (password, key material) are never returned in GET.
// Register via bb_registry at regular tier (order 5).
//
// Host twin: platform/host/bb_mqtt_routes/bb_mqtt_routes_host.c
#pragma once
#include "bb_core.h"
#include "bb_mqtt.h"

#ifdef __cplusplus
extern "C" {
#endif

// Register GET+PATCH /api/mqtt with the HTTP server.
// Called automatically when CONFIG_BB_MQTT_ROUTES_AUTOREGISTER=y.
bb_err_t bb_mqtt_routes_init(bb_http_handle_t server);

// Set the client handle reference used for connection state in GET /api/mqtt.
// Pass a pointer to the module-level bb_mqtt_t handle (or NULL to clear).
void bb_mqtt_routes_set_client(bb_mqtt_t *ref);

#ifdef BB_MQTT_ROUTES_TESTING

// Reset route state for test isolation.
void bb_mqtt_routes_reset_for_test(void);

// Expose GET and PATCH handlers for test invocation.
bb_err_t bb_mqtt_routes_get_handler_for_test(bb_http_request_t *req);
bb_err_t bb_mqtt_routes_patch_handler_for_test(bb_http_request_t *req);

#endif /* BB_MQTT_ROUTES_TESTING */

#ifdef __cplusplus
}
#endif
