// bb_udp_client — private helpers shared by the host and ESP-IDF backends.
// Not part of the public API; included via PRIV_INCLUDE_DIRS "src" from
// platform/{host,espidf}/bb_udp_client/*.c only.
#pragma once

#include "bb_udp_client.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Kconfig bridge (canonical two-step pattern; see bb_clock.h / bb_net_health.h).
// On ESP-IDF, Kconfig generates CONFIG_BB_UDP_* symbols. Bridge them to the
// resolved BB_UDP_CLIENT_* macros here so both backends (host, espidf) read
// one already-resolved definition instead of each re-deriving its own ad-hoc
// fallback.
// ---------------------------------------------------------------------------

#ifdef ESP_PLATFORM
#  ifdef CONFIG_BB_UDP_PORT
#    define BB_UDP_CLIENT_PORT CONFIG_BB_UDP_PORT
#  endif
#  ifdef CONFIG_BB_UDP_BROADCAST
#    define BB_UDP_CLIENT_BROADCAST CONFIG_BB_UDP_BROADCAST
#  endif
#endif

#ifndef BB_UDP_CLIENT_PORT
#define BB_UDP_CLIENT_PORT 9109
#endif
#ifndef BB_UDP_CLIENT_BROADCAST
#define BB_UDP_CLIENT_BROADCAST 0
#endif

/**
 * Load bb_udp_client_cfg_t from NVS namespace "bb_udp", falling back to
 * Kconfig defaults (CONFIG_BB_UDP_PORT / CONFIG_BB_UDP_BROADCAST) for any
 * unset key.
 */
void bb_udp_client_priv_load_from_nvs(bb_udp_client_cfg_t *out);

/** Persist bb_udp_client_cfg_t to NVS namespace "bb_udp". */
void bb_udp_client_priv_save_to_nvs(const bb_udp_client_cfg_t *cfg);

#ifdef __cplusplus
}
#endif
