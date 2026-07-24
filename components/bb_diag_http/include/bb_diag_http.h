#pragma once

/**
 * @brief bb_diag_http — the aggregate HTTP surface for bb_diag (B1-1153, KB
 * 1477). bb_diag itself is a pure SSOT (reset-reason latch, panic capture,
 * section registry, gather/bind seams) with zero bb_http_server dependency;
 * every route this component registers -- reads and controls alike -- was
 * relocated here VERBATIM (no behavior change) from bb_diag's own prior
 * platform/espidf/bb_diag/ files. A board that wants ONLY panic/boot
 * diagnostics (e.g. logged over a debug UART, or polled via bb_diag's own
 * getters) can compose bb_diag without this component and pay nothing for
 * an HTTP surface.
 *
 * Two independent Kconfig-gated route surfaces (own Kconfig menu below),
 * unchanged in shape from their prior home:
 *   - CONFIG_BB_DIAG_ROUTES: legacy exact routes -- GET/DELETE
 *     /api/diag/boot, GET/DELETE /api/diag/panic, GET /api/diag/coredump,
 *     GET /api/diag/heap-check, GET /api/diag/tasks, GET /api/diag/sockets.
 *   - CONFIG_BB_DIAG_SECTIONS: the generic GET /api/diag/<section> wildcard
 *     dispatcher serving whichever section a caller has registered via
 *     bb_diag_register_section() (bb_diag_section.h, stays in bb_diag).
 *
 * bb_diag_boot_render_envelope() always builds regardless of either
 * Kconfig -- same as its prior home (components/bb_diag/bb_diag_boot_wire.c),
 * host-testable directly.
 *
 * B1-1154 (KB 1477): the former bb_storage_http component was dissolved into
 * this one -- DELETE /api/diag/storage (bb_storage_http_routes_init(),
 * always builds) and POST /api/diag/factory-reset
 * (bb_storage_http_factory_reset_routes_init(), gated behind
 * CONFIG_BB_STORAGE_HTTP_FACTORY_RESET) are declared in bb_storage_http.h
 * (this component's own second public header, unmerged into this file to
 * keep the mechanical move a pure relocation).
 *
 * B1-1155 (KB 1477): the former bb_log_http component was dissolved into
 * this one -- GET/POST /api/log/level (bb_log_register_routes_init(),
 * always builds; CONFIG_BB_LOG_ROUTES internally gates registration) are
 * declared in bb_log_http.h (this component's own third public header,
 * unmerged into this file to keep the mechanical move a pure relocation).
 */

#include "bb_diag.h"
#include "bb_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Renders the "diag.boot" bb_data binding onto `req` as a {"ts_ms":N,
 * "data":{...}} envelope: "data" is bb_diag_boot_wire_desc's JSON rendering
 * (byte-identical to bb_data_render()'s own output), "ts_ms" is
 * bb_clock_now_ms64() read at render time -- this key's gather
 * (bb_diag_boot_gather(), bb_diag_boot_wire.h, bb_diag) has no notion of a
 * wire-carried sample time (it widens whatever bb_cache currently holds;
 * bb_data_render() itself never surfaces a timestamp), so "ts_ms" here means
 * "when this response was generated", not "when the underlying value was
 * sampled". Portable (compiles host + ESP-IDF; only bb_diag_boot_bind()
 * needs to have already run) -- host-testable directly via
 * bb_http_host_capture_begin/end (mirrors test_bb_http_json_obj_stream.c).
 *
 * Returns BB_ERR_INVALID_ARG if `req` is NULL. Otherwise propagates
 * bb_data_render()'s own error (e.g. BB_ERR_NOT_FOUND if "diag.boot" isn't
 * bound) or any bb_http_resp_json_obj_* stream error.
 */
bb_err_t bb_diag_boot_render_envelope(bb_http_request_t *req);

#if CONFIG_BB_DIAG_ROUTES
#ifdef ESP_PLATFORM

/**
 * Registry hook — registers GET/DELETE /api/diag/panic, GET /api/diag/boot,
 * plus heap-check/tasks/sockets diagnostics.
 */
// bbtool:init tier=regular fn=bb_diag_routes_init server=true
bb_err_t bb_diag_routes_init(bb_http_handle_t server);

#endif /* ESP_PLATFORM */
#endif /* CONFIG_BB_DIAG_ROUTES */

#if CONFIG_BB_DIAG_SECTIONS
#ifdef ESP_PLATFORM

// Registers the GET /api/diag/<section> wildcard route on `server`, dispatching to
// whichever section a request names (platform/espidf/bb_diag_http/
// bb_diag_http_section_dispatch.c). ESP-IDF only -- there is no host server
// to register against.
// bbtool:init tier=regular fn=bb_diag_sections_init server=true
bb_err_t bb_diag_sections_init(bb_http_handle_t server);

#endif /* ESP_PLATFORM */
#endif /* CONFIG_BB_DIAG_SECTIONS */

#ifdef __cplusplus
}
#endif
