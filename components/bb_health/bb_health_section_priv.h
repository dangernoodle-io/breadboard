#pragma once
// Private shared header for the bb_health_section composer registry
// (B1-1096 PR-1). Kconfig-bridge byte budget -- factored out of
// bb_health_section.c so a host test can reference
// BB_HEALTH_SECTION_SCRATCH_BYTES directly to build an oversized fixture
// (mirrors bb_diag_section_priv.h's pattern). No ESP-IDF or FreeRTOS types
// here.
// Included by:
//   - components/bb_health/bb_health_section.c
//   - test/test_host/test_bb_health_section.c

#include "bb_health_section.h"

// Shared per-section render scratch buffer size -- the future composer's
// (B1-1054 PR-5) request-scoped snapshot scratch, ONE section rendered at a
// time (sequentially, not all BB_HEALTH_SECTION_TABLE_CAP slots' worth
// simultaneously). bb_health_section_register() rejects (BB_ERR_NO_SPACE)
// any section whose snap_desc->snap_size exceeds this at registration
// time, so a future dispatched request can never overrun the buffer it
// renders into.
//
// Sizing (B1-1096 -- this is the load-bearing number for PR-5's stack
// budget, measured rather than padded by convenience):
//
//   Measured struct sizes (host, LP64; ESP32 is ILP32 so real sizes are
//   the same or SMALLER -- e.g. any pointer-bearing field shrinks from 8 to
//   4 bytes -- making every number below a conservative upper bound):
//     bb_health_wire_t                     96 B  (ROOT entry -- NOT a
//                                                  registered section; the
//                                                  future composer's root
//                                                  slice, excluded from this
//                                                  budget)
//     bb_mqtt_client_health_snap_t         32 B  (bool + 3x int64/uint64,
//                                                  B1-1040)
//     bb_tcp_client_health_snap_t          32 B  (same 4-field shape,
//                                                  B1-1041)
//     bb_http_client_session_health_snap_t 32 B  (same 4-field shape,
//                                                  B1-1041; not registered
//                                                  today, bounds the
//                                                  plausible future set)
//     bb_temp's section shape (bool present + float soc_c)   8 B
//                                                (ESTIMATED -- no struct
//                                                 exists yet in
//                                                 components/bb_temp, only a
//                                                 JSON-shape doc comment)
//
//   Realistic total across every section registered today (mqtt + temp,
//   B1-1096 design notes) is ~40 B; worst case at BB_HEALTH_SECTION_TABLE_CAP
//   (8) full of the largest known shape (32 B) is 256 B.
//
//   Budget check against CONFIG_BB_HTTP_TASK_STACK_SIZE (6144 B total,
//   components/bb_http_server/Kconfig.projbuild): this composer's own path
//   must independently fit under 6144 B -- baseline call-frame overhead +
//   128 B scratch + ~8 B/frame overhead per nested call, checked on its OWN,
//   never summed against another handler. The WS-connect snapshot-on-connect
//   path's ~3.5 KB claim (B1-589/B1-592) is a SEPARATE top-level route
//   dispatch on the esp_http_server worker task -- sequential with, never
//   nested inside or concurrent with, a GET /api/health dispatch on the same
//   task type -- so it is an ALTERNATE PEAK, not a claim to subtract from a
//   shared budget: baseline + max(3584, 768 + 128) < 6144, NOT
//   6144 - 3584 - 768. It's noted here only as a reminder that ANY handler
//   registered on this task type must independently clear the same 6144 B
//   ceiling.
//
//   At 128 B/section x BB_HEALTH_SECTION_TABLE_CAP (8) = 1024 B worst case
//   (every slot registered AND at the scratch cap simultaneously -- never
//   true in practice, since real sections top out at 32 B) -- comfortably
//   under budget, a 4x headroom factor over the largest real snapshot
//   measured above (32 B), enough for a section to add a few more fields
//   without hitting the reject, without being padded past what the budget
//   affords.
//
//   The earlier subtractive framing (treating the WS-connect figure as a
//   shared claim to subtract) was over-conservative -- the safe direction,
//   since it understated the true remaining budget -- so it changes no
//   number here: 128 B/8 slots remains the justified choice, not the
//   originally-proposed 256 B (which would have been tighter for no
//   measured benefit).
#ifdef CONFIG_BB_HEALTH_SECTION_SCRATCH_BYTES
#define BB_HEALTH_SECTION_SCRATCH_BYTES CONFIG_BB_HEALTH_SECTION_SCRATCH_BYTES
#else
#define BB_HEALTH_SECTION_SCRATCH_BYTES 128
#endif
