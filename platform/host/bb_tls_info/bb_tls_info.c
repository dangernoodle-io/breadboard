// bb_tls_info — PRE_HTTP companion that registers per-transport TLS
// capability flags into /api/info capabilities[].
//
// Compiled for both host (test) and ESP-IDF targets.
//
// HOST BUILD NOTE: the three CONFIG_BB_*_TLS_* gates are defined ON
// (-DCONFIG_BB_MQTT_TLS_ENABLE=1 etc.) in the native PlatformIO env so that
// all TLS code paths compile and are exercised by host tests.  On ESP-IDF
// the Kconfig defaults are OFF (n); enable via menuconfig or sdkconfig.
#include "bb_tls_info.h"
#include "bb_info.h"

void bb_tls_info_register(void)
{
#if CONFIG_BB_MQTT_TLS_ENABLE
    bb_info_register_capability("mqtt_tls");
#endif
#if CONFIG_BB_HTTP_TLS_ENABLE
    bb_info_register_capability("http_tls");
#endif
#if CONFIG_BB_TLS_MUTUAL_ENABLE
    bb_info_register_capability("mutual_tls");
#endif
}

// PRE_HTTP-tier init wrapper for the bb_app_init() composition root
// (bbtool:init marker in bb_tls_info.h), not self-registered.
void bb_tls_info_pre_http_init(void)
{
    bb_tls_info_register();
}
