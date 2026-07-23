#pragma once

// bb_diag_meminfo -- the "meminfo" bb_diag section (GET /api/diag/meminfo),
// the heap-reconciliation cluster of B1-diag-dissolution. A thin
// bb_diag_fill_fn adapter over bb_meminfo's own SSOT snapshot
// (bb_meminfo_heap_snap_fill(), components/bb_meminfo/include/
// bb_meminfo_heap_snap.h) -- this file owns no heap-reading logic of its
// own, only the section-registry wiring that used to be hand-rolled per
// firmware (see examples/floor/main/floor_app.c's now-removed
// diag_fill_meminfo local adapter).
//
// The legacy /api/diag/heap exact route (platform/espidf/bb_diag/
// bb_diag_routes.c) has been DELETED -- its `exec` region and per-region
// `allocated` field were folded into bb_meminfo's own snapshot
// (bb_meminfo.h/bb_meminfo_heap_snap.h) rather than duplicated here, so
// /api/diag/meminfo is now the single unified memory report. The route's
// `?check=true` integrity walk (heap_caps_check_integrity_all, a
// side-effecting action, semantically wrong for a passive stats section) was
// relocated to its own standalone GET /api/diag/heap-check route
// (platform/espidf/bb_diag_http/bb_diag_http_routes.c).

#include "bb_diag_section.h"
#include "bb_serialize.h"

#ifdef __cplusplus
extern "C" {
#endif

// Fill hook (bb_diag_fill_fn signature) -- pure/portable, delegates to
// bb_meminfo_heap_snap_fill(). `args` is unused (this section declares no
// query_keys). Returns BB_ERR_INVALID_ARG if dst is NULL; otherwise
// propagates bb_meminfo_heap_snap_fill()'s own return.
bb_err_t bb_diag_meminfo_fill(void *dst, const bb_diag_fill_args_t *args);

#ifdef ESP_PLATFORM
// Registers this section as "meminfo" (GET /api/diag/meminfo) via
// bb_diag_register_section(). Composition-time-only, once -- see
// bb_diag_storage_nvs_register()'s doc comment for the same contract.
// bbtool:init tier=regular fn=bb_diag_meminfo_register
bb_err_t bb_diag_meminfo_register(void);

#endif /* ESP_PLATFORM */

#ifdef __cplusplus
}
#endif
