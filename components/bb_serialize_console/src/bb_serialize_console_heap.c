// Heap-over-serial one-shot milestone report: bb_meminfo_get() (the
// canonical heap_caps reader SSOT, KB #698/#699/#693) -> descriptor walk via
// bb_serialize_console_render() -> bb_log_i(). No periodic task -- a
// consumer calls bb_serialize_console_heap_report() at its own milestones.

#include "bb_serialize_console.h"

#include "bb_log.h"
#include "bb_meminfo.h"

#include <stddef.h>
#include <string.h>

static const char *TAG = "bb_serialize_console_heap";

// ---------------------------------------------------------------------------
// Kconfig bridge -- CONFIG_BB_SERIALIZE_CONSOLE_LINE_MAX_BYTES -> a C
// default. Never shadow the generated symbol with a bare #ifndef.
// ---------------------------------------------------------------------------
#ifdef CONFIG_BB_SERIALIZE_CONSOLE_LINE_MAX_BYTES
#define BB_SERIALIZE_CONSOLE_LINE_MAX_BYTES CONFIG_BB_SERIALIZE_CONSOLE_LINE_MAX_BYTES
#else
#define BB_SERIALIZE_CONSOLE_LINE_MAX_BYTES 160
#endif

static const bb_serialize_field_t s_heap_fields[] = {
    { .key = "internal_free", .type = BB_TYPE_U64,
      .offset = offsetof(bb_serialize_console_heap_snap_t, internal_free) },
    { .key = "internal_min_ever_free", .type = BB_TYPE_U64,
      .offset = offsetof(bb_serialize_console_heap_snap_t, internal_min_ever_free) },
    { .key = "internal_largest_free_block", .type = BB_TYPE_U64,
      .offset = offsetof(bb_serialize_console_heap_snap_t, internal_largest_free_block) },
    { .key = "spiram_free", .type = BB_TYPE_U64,
      .offset = offsetof(bb_serialize_console_heap_snap_t, spiram_free) },
    { .key = "dma_free", .type = BB_TYPE_U64,
      .offset = offsetof(bb_serialize_console_heap_snap_t, dma_free) },
    { .key = "esp_min_free_heap", .type = BB_TYPE_U64,
      .offset = offsetof(bb_serialize_console_heap_snap_t, esp_min_free_heap) },
};

const bb_serialize_desc_t bb_serialize_console_heap_desc = {
    .type_name = "bb_serialize_console_heap_snap_t",
    .fields = s_heap_fields,
    .n_fields = sizeof(s_heap_fields) / sizeof(s_heap_fields[0]),
    .snap_size = sizeof(bb_serialize_console_heap_snap_t),
};

bb_err_t bb_serialize_console_heap_gather(void *dst, void *ctx)
{
    (void)ctx;
    if (!dst) return BB_ERR_INVALID_ARG;

    bb_meminfo_snapshot_t m;
    bb_err_t rc = bb_meminfo_get(&m);
    if (rc != BB_OK) return rc;  // LCOV_EXCL_BR_LINE -- bb_meminfo_get() only fails on a NULL out, never passed here

    bb_serialize_console_heap_snap_t *out = (bb_serialize_console_heap_snap_t *)dst;
    out->internal_free               = (uint64_t)m.internal.free;
    out->internal_min_ever_free      = (uint64_t)m.internal.min_ever_free;
    out->internal_largest_free_block = (uint64_t)m.internal.largest_free_block;
    out->spiram_free                 = (uint64_t)m.spiram.free;
    out->dma_free                    = (uint64_t)m.dma.free;
    out->esp_min_free_heap           = (uint64_t)m.esp_min_free_heap;
    return BB_OK;
}

void bb_serialize_console_heap_report(const char *label)
{
    const char *lbl = label ? label : "?";

    bb_serialize_console_heap_snap_t snap;
    memset(&snap, 0, sizeof(snap));

    // dst is always this function's own non-NULL stack snapshot -- gather
    // can only fail here via bb_meminfo_get()'s own internal error path,
    // which the host/ESP-IDF backends never take today; kept as a real
    // checked branch (not asserted away) since bb_meminfo_get()'s contract
    // does not promise infallibility.
    bb_err_t rc = bb_serialize_console_heap_gather(&snap, NULL);
    // LCOV_EXCL_START -- bb_meminfo_get() never fails on a non-NULL out today
    if (rc != BB_OK) {
        bb_log_e(TAG, "%s: heap gather failed (%d)", lbl, (int)rc);
        return;
    }
    // LCOV_EXCL_STOP

    char line[BB_SERIALIZE_CONSOLE_LINE_MAX_BYTES];
    size_t out_len = 0;
    bb_err_t rrc = bb_serialize_console_render(&bb_serialize_console_heap_desc, &snap,
                                                line, sizeof(line), &out_len);
    // LCOV_EXCL_START -- unreachable: line/sizeof(line) are always valid (buf non-NULL, cap>0)
    if (rrc != BB_OK) {
        bb_log_e(TAG, "%s: heap render failed (%d)", lbl, (int)rrc);
        return;
    }
    // LCOV_EXCL_STOP

    bb_log_i(TAG, "%s heap %s", lbl, line);
}
