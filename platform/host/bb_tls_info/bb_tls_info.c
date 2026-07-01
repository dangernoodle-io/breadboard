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

#ifndef CONFIG_BB_TLS_INFO_AUTOREGISTER
#define CONFIG_BB_TLS_INFO_AUTOREGISTER 0
#endif

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

#if CONFIG_BB_TLS_INFO_AUTOREGISTER
#include "bb_init.h"

static bb_err_t tls_info_pre_http_init(void)
{
    bb_tls_info_register();
    return BB_OK;
}

BB_INIT_REGISTER_PRE_HTTP(bb_tls_info, tls_info_pre_http_init);
#endif /* CONFIG_BB_TLS_INFO_AUTOREGISTER */
