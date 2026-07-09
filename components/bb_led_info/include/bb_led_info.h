#pragma once

#include "bb_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * bb_led_info — satellite component that registers a /api/info extender
 * reporting LED state from bb_led_primary().
 *
 * Include this component (REQUIRES bb_led_info) when you want the /api/info
 * response to include a nested "led" object:
 *   { present, type, count, rgb }
 *
 * Call bb_led_register_info() before bb_http_server_start (before the
 * extender table is frozen). The consumer must also set bb_led_set_primary()
 * before calling this if a present:true reading is desired.
 *
 * The "enabled" field reflects bb_led_enabled(primary) — the consumer-
 * controlled logical on/off flag (default true).  It does not affect hardware;
 * set it via bb_led_set_enabled() to report the LED's logical state.
 *
 * Presence of this satellite component in the build (via REQUIRES) is the
 * opt-in mechanism — no Kconfig gate is needed.
 */
void bb_led_register_info(void);

/**
 * Registry hook — calls bb_led_register_info(). server is unused
 * (bb_led_info has no HTTP routes of its own, only a /api/info section).
 */
// bbtool:init tier=regular fn=bb_led_info_autoregister_init server=true
bb_err_t bb_led_info_autoregister_init(bb_http_handle_t server);

#ifdef __cplusplus
}
#endif
