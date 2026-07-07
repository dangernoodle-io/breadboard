// bb_sink_udp — private helpers shared by the host and ESP-IDF backends.
// Not part of the public API; included via PRIV_INCLUDE_DIRS "src" from
// platform/{host,espidf}/bb_sink_udp/*.c only.
#pragma once

#include "bb_sink_udp.h"
#include "bb_udp_frame.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Kconfig bridge (canonical two-step pattern; see bb_clock.h / bb_net_health.h).
// On ESP-IDF, Kconfig generates CONFIG_BB_SINK_UDP_* symbols. Bridge them to
// the resolved BB_SINK_UDP_* macros here so all three backends (common, host,
// espidf) read one already-resolved definition instead of each re-deriving
// its own ad-hoc fallback.
//
// Only MTU remains here — the destination config (PORT/BROADCAST/HOST) moved
// to bb_udp_client along with the socket transport it gates (KB#702/#710).
// ---------------------------------------------------------------------------

#ifdef ESP_PLATFORM
#  ifdef CONFIG_BB_SINK_UDP_MTU
#    define BB_SINK_UDP_MTU CONFIG_BB_SINK_UDP_MTU
#  endif
#endif

#ifndef BB_SINK_UDP_MTU
#define BB_SINK_UDP_MTU 1400
#endif

/**
 * Encode one TELEMETRY frame (next atomic seq, flags=0) into buf.
 * On success returns the encoded length (same contract as
 * bb_udp_frame_encode). On failure (frame doesn't fit buf_cap) increments
 * the shared dropped counter (bb_sink_udp_dropped()) and returns -1.
 */
int bb_sink_udp_priv_encode(const char *topic, const char *payload, int len,
                             uint8_t *buf, int buf_cap);

#ifdef BB_SINK_UDP_TESTING
/** Reset the shared seq/dropped counters (test isolation). */
void bb_sink_udp_priv_test_reset(void);
#endif

#ifdef __cplusplus
}
#endif
