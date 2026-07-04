#pragma once
// bb_mdns_refresh_decision — pure decision logic for whether the periodic
// browse-refresh (browse_refresh_work_fn, platform/espidf/bb_mdns/bb_mdns.c)
// may recreate a browse subscription after a best-effort mdns_browse_delete
// (B1-539). No ESP-IDF/FreeRTOS types — host-testable in isolation, mirroring
// sse_pool_reclaim_decision.h.
//
// Under mDNS action-queue pressure, mdns_browse_delete's ENQUEUE can fail
// (ESP_ERR_NO_MEM) even though the existing browse is still live. Recreating
// unconditionally in that case issues mdns_browse_new against a browse that
// was never actually torn down, orphaning the old browse handle. Skipping
// the recreate for that one cycle and retrying on the next tick avoids the
// orphan; the delete succeeding, or the browse already being gone, are both
// safe to recreate on.

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BB_MDNS_REFRESH_DELETE_OK = 0,   // delete enqueued/applied successfully
    BB_MDNS_REFRESH_DELETE_OTHER,    // ESP_FAIL (mDNS not running / never started) or any other non-NO_MEM esp_err_t; recreate is safe
    BB_MDNS_REFRESH_DELETE_NO_MEM,   // delete enqueue failed: queue full / no memory
} bb_mdns_refresh_delete_rc_t;

// Pure state -> action mapping. Recreate is safe when the delete succeeded
// or the browse was already gone; skip (retry next tick) when the delete
// enqueue failed for lack of memory/queue space.
bool bb_mdns_refresh_should_recreate(bb_mdns_refresh_delete_rc_t rc);

#ifdef __cplusplus
}
#endif
