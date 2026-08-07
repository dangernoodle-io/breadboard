#pragma once
// smoke_data_notify -- B1-1451 (epic B1-1123) on-device proof: a producer
// task path (via bb_data_http_notify_push()) delivering an EVENT-kind push
// through bb_data_http's shared ring, wired up via three runtime-triggered
// HTTP routes so both the delivery-latency claim and the two-task ring
// contention stress can be observed OFF-BOARD. See smoke_data_notify.c for
// the full rationale + exact off-board verification recipe. examples/
// smoke-only app code, never a library component -- mirrors
// smoke_core_claim.h's own shape.
//
// The `// bbtool:init` markers that wire these entry points into the
// generated composition root live in bb_wire.h (the consumer manifest,
// examples/smoke/main/bb_wire.h), NOT here -- see smoke_core_claim.h's
// identical comment for why.
#include "bb_core.h"
#include "bb_http.h"

#ifdef __cplusplus
extern "C" {
#endif

// Binds the "smoke_notify" EVENT-kind bb_data key (wraps bb_data_bind()).
// No-op (returns BB_OK, no key bound) when CONFIG_BB_SMOKE_DATA_NOTIFY is
// off -- see smoke_data_notify.c's file header for why the bind/attach
// pair is itself gated (B1-1451 review fix: a default smoke build must not
// permanently consume a bb_data binding-table slot for a feature it never
// uses).
bb_err_t smoke_data_notify_bind(void);

// Attaches "smoke_notify" into bb_data_http's own attach table under topic
// "smoke.notify" (wraps bb_data_http_attach()). No-op (returns BB_OK,
// nothing attached) when CONFIG_BB_SMOKE_DATA_NOTIFY is off -- see
// smoke_data_notify_bind()'s doc above.
bb_err_t smoke_data_notify_attach(void);

// Registers POST /api/smoke/notify-push, POST /api/smoke/tick-push, and
// POST /api/smoke/notify-stress on `server`. No-op (returns BB_OK, no route
// registered) when CONFIG_BB_SMOKE_DATA_NOTIFY is off.
bb_err_t smoke_data_notify_routes_init(bb_http_handle_t server);

#ifdef __cplusplus
}
#endif
