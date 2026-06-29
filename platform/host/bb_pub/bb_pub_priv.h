#pragma once
// bb_pub private internal helpers.
// These functions must be called with s_tick_lock already held.
// External callers must use the public bb_pub_get_status / bb_pub_sink_info
// which acquire the lock themselves.
#include "bb_pub.h"

bb_err_t bb_pub_get_status_nolock(bb_pub_status_t *out);
bb_err_t bb_pub_sink_info_nolock(int i, const char **out_transport, bool *out_tls);
