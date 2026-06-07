#pragma once

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
 * Presence of this satellite component in the build (via REQUIRES) is the
 * opt-in mechanism — no Kconfig gate is needed.
 */
void bb_led_register_info(void);

#ifdef __cplusplus
}
#endif
