// bb_tcp_client — private helpers shared by the host and ESP-IDF backends.
// Not part of the public API; included via PRIV_INCLUDE_DIRS "src" from
// platform/{host,espidf}/bb_tcp_client/*.c only.
#pragma once

#include "bb_tcp_client.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Kconfig bridge (canonical two-step pattern; see bb_clock.h / bb_udp_client's
// bb_udp_client_priv.h). On ESP-IDF, Kconfig generates CONFIG_BB_TCP_* symbols.
// Bridge them to the resolved BB_TCP_* macros here so both backends (host,
// espidf) read one already-resolved definition instead of each re-deriving
// its own ad-hoc fallback. Never shadow the generated CONFIG_ symbol with a
// bare #ifndef.
// ---------------------------------------------------------------------------

#ifdef ESP_PLATFORM
#  ifdef CONFIG_BB_TCP_CLIENT_MAX_INSTANCES
#    define BB_TCP_CLIENT_MAX_INSTANCES CONFIG_BB_TCP_CLIENT_MAX_INSTANCES
#  endif
#  ifdef CONFIG_BB_TCP_CONNECT_TIMEOUT_MS
#    define BB_TCP_CONNECT_TIMEOUT_MS CONFIG_BB_TCP_CONNECT_TIMEOUT_MS
#  endif
#  ifdef CONFIG_BB_TCP_IO_TIMEOUT_MS
#    define BB_TCP_IO_TIMEOUT_MS CONFIG_BB_TCP_IO_TIMEOUT_MS
#  endif
#  ifdef CONFIG_BB_TCP_TLS_DEFAULT
#    define BB_TCP_TLS_DEFAULT CONFIG_BB_TCP_TLS_DEFAULT
#  endif
#endif

#ifndef BB_TCP_CLIENT_MAX_INSTANCES
#define BB_TCP_CLIENT_MAX_INSTANCES 1
#endif
#ifndef BB_TCP_CONNECT_TIMEOUT_MS
#define BB_TCP_CONNECT_TIMEOUT_MS 5000
#endif
#ifndef BB_TCP_IO_TIMEOUT_MS
#define BB_TCP_IO_TIMEOUT_MS 5000
#endif
#ifndef BB_TCP_TLS_DEFAULT
#define BB_TCP_TLS_DEFAULT 0
#endif

/**
 * Load bb_tcp_client_cfg_t host/port/tls from NVS namespace `ns`, falling
 * back to Kconfig defaults (host="", port=0, tls=BB_TCP_TLS_DEFAULT) for any
 * unset key. connect_timeout_ms/io_timeout_ms/cert fields are left zeroed —
 * cfg's caller-supplied-only fields never round-trip through NVS. `ns` is
 * borrowed — used only for the duration of this call.
 */
void bb_tcp_client_priv_load_from_nvs(const char *ns, bb_tcp_client_cfg_t *out);

/** Persist host/port/tls (only) to NVS namespace `ns` (borrowed). */
void bb_tcp_client_priv_save_to_nvs(const char *ns, const bb_tcp_client_cfg_t *cfg);

/**
 * Report ok/fail to the shared bb_transport_health "tcp" AUTHORITATIVE slot,
 * registering it lazily on first call. Shared by both backends so the
 * reporting policy (see bb_tcp_client.h) lives in exactly one place.
 */
void bb_tcp_client_priv_health_report(bool ok);

#ifdef BB_TCP_CLIENT_TESTING
/** Clear the cached transport_health slot handle (test isolation only). */
void bb_tcp_client_priv_reset_health_for_test(void);
#endif

#ifdef __cplusplus
}
#endif
