#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/*
 * bb_tls_info — PRE_HTTP companion that registers per-transport TLS
 * capability flags into the /api/info capabilities[] array.
 *
 * Capabilities registered (each only when its compile gate is ON):
 *   "mqtt_tls"   — CONFIG_BB_MQTT_TLS_ENABLE=y
 *   "http_tls"   — CONFIG_BB_HTTP_TLS_ENABLE=y
 *   "mutual_tls" — CONFIG_BB_TLS_MUTUAL_ENABLE=y
 *
 * Must be called at PRE_HTTP tier (before bb_info_init freeze at regular
 * order 2). The auto-register companion (CONFIG_BB_TLS_INFO_AUTOREGISTER,
 * default y) handles this automatically.  The manual call is provided for
 * consumers that disable autoregister and control registration order.
 *
 * Registration is idempotent: bb_info_register_capability silently ignores
 * duplicates (string-compare dedup).
 */
void bb_tls_info_register(void);

#ifdef __cplusplus
}
#endif
