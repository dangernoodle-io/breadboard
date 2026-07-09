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
 * order 2). bb_tls_info_pre_http_init (below) wraps this call for the
 * bb_app_init() composition root. The manual call is provided for
 * consumers that want to control registration order directly.
 *
 * Registration is idempotent: bb_info_register_capability silently ignores
 * duplicates (string-compare dedup).
 */
void bb_tls_info_register(void);

// bbtool:init tier=pre_http fn=bb_tls_info_pre_http_init
void bb_tls_info_pre_http_init(void);

#ifdef __cplusplus
}
#endif
