#include "bb_meminfo.h"
#include "bb_registry.h"

#include <stdio.h>
#include <string.h>

// Host has no heap_caps_* equivalent — every field zeroed, mirroring
// bb_board_host.c's existing host-stub convention.
bb_err_t bb_meminfo_get(bb_meminfo_snapshot_t *out)
{
    if (!out) return BB_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    return BB_OK;
}

// Pure formatting — identical on host and ESP-IDF (duplicated in each
// platform impl per bb_meminfo's own convention; see bb_meminfo.h).
int bb_meminfo_format(const bb_meminfo_snapshot_t *snap, char *buf, size_t len)
{
    if (!snap || !buf || len == 0) return 0;
    return snprintf(buf, len,
                     "heap_int_free=%u int_min=%u int_largest=%u "
                     "spiram_free=%u dma_free=%u esp_min_free=%u",
                     (unsigned)snap->internal.free,
                     (unsigned)snap->internal.min_ever_free,
                     (unsigned)snap->internal.largest_free_block,
                     (unsigned)snap->spiram.free,
                     (unsigned)snap->dma.free,
                     (unsigned)snap->esp_min_free_heap);
}

// ---------------------------------------------------------------------------
// bb_memreport — arena region registry + aggregator. The registry walk is
// identical host+espidf (bb_mem_arena works on host); only bb_meminfo_get's
// heap portion differs — duplicated in each platform impl per bb_meminfo's
// own convention (see bb_meminfo_format above, and bb_meminfo.h).
// ---------------------------------------------------------------------------

BB_REGISTRY_DEFINE_TAGGED(s_memreport_registry, BB_MEMREPORT_MAX_REGIONS, "bb_meminfo");

bb_err_t bb_memreport_register_arena(const char *name, bb_mem_arena_t a)
{
    if (!name || !a) return BB_ERR_INVALID_ARG;
    return bb_registry_register(&s_memreport_registry, name, (void *)a);
}

void bb_memreport_deregister(const char *name)
{
    if (!name) return;
    (void)bb_registry_deregister(&s_memreport_registry, name);
}

static void memreport_collect_cb(const char *name, void *value, void *ctx)
{
    bb_memreport_snapshot_t *out = (bb_memreport_snapshot_t *)ctx;
    if (out->region_count >= BB_MEMREPORT_MAX_REGIONS) return; // LCOV_EXCL_BR_LINE — unreachable: bb_registry enforces this bound at register time

    bb_mem_arena_t a = (bb_mem_arena_t)value;
    bb_memreport_region_t *r = &out->regions[out->region_count];
    memset(r, 0, sizeof(*r));
    strncpy(r->name, name, sizeof(r->name) - 1);
    r->name[sizeof(r->name) - 1] = '\0';

    size_t free_bytes  = bb_mem_arena_free_bytes(a);
    size_t total_bytes = bb_mem_arena_size(a);
    r->free_bytes = free_bytes;
    r->used_bytes = (total_bytes > free_bytes) ? (total_bytes - free_bytes) : 0;

    bb_mem_arena_stats_t stats;
    bb_mem_arena_get_stats(a, &stats);
    r->peak_used_bytes = stats.peak_offset;
    r->alloc_count     = stats.alloc_count;
    r->free_count      = stats.free_count;
    r->alloc_failed    = stats.alloc_failed;

    out->region_count++;
}

void bb_memreport_get(bb_memreport_snapshot_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    bb_meminfo_get(&out->heap);
    bb_registry_foreach(&s_memreport_registry, memreport_collect_cb, out);
}

// Truncation-safe chained-append: total accumulates the untruncated
// snprintf-semantics length (may exceed len); pos is clamped to [0, len] so
// buf+pos / len-pos always stay in bounds for the next append.
int bb_memreport_format(const bb_memreport_snapshot_t *snap, char *buf, size_t len)
{
    if (!snap || !buf || len == 0) return 0;

    int total = bb_meminfo_format(&snap->heap, buf, len);
    if (total < 0) return total; // LCOV_EXCL_BR_LINE — snprintf never returns <0 for this format

    size_t pos = ((size_t)total < len) ? (size_t)total : len;
    int n = snprintf(buf + pos, len - pos, " bss=%u",
                      (unsigned)snap->heap.dram_static_bytes);
    if (n < 0) return n; // LCOV_EXCL_BR_LINE — snprintf never returns <0 for this format
    total += n;
    pos = ((size_t)total < len) ? (size_t)total : len;

    for (uint16_t i = 0; i < snap->region_count; i++) {
        const bb_memreport_region_t *r = &snap->regions[i];
        n = snprintf(buf + pos, len - pos, " %s=%u/%u/%u",
                      r->name,
                      (unsigned)r->free_bytes,
                      (unsigned)r->peak_used_bytes,
                      (unsigned)r->used_bytes);
        if (n < 0) return n; // LCOV_EXCL_BR_LINE — snprintf never returns <0 for this format
        total += n;
        pos = ((size_t)total < len) ? (size_t)total : len;
    }

    return total;
}
