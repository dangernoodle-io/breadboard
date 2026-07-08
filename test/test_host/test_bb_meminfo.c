#include "unity.h"
#include "bb_meminfo.h"
#include "bb_mem_arena.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

void test_bb_meminfo_get_rejects_null(void)
{
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_meminfo_get(NULL));
}

void test_bb_meminfo_get_host_zeroes_snapshot(void)
{
    bb_meminfo_snapshot_t m;
    memset(&m, 0xAA, sizeof(m));

    TEST_ASSERT_EQUAL(BB_OK, bb_meminfo_get(&m));

    TEST_ASSERT_EQUAL_size_t(0, m.default_region.free);
    TEST_ASSERT_EQUAL_size_t(0, m.default_region.min_ever_free);
    TEST_ASSERT_EQUAL_size_t(0, m.default_region.largest_free_block);
    TEST_ASSERT_EQUAL_size_t(0, m.default_region.total);

    TEST_ASSERT_EQUAL_size_t(0, m.internal.free);
    TEST_ASSERT_EQUAL_size_t(0, m.internal.min_ever_free);
    TEST_ASSERT_EQUAL_size_t(0, m.internal.largest_free_block);
    TEST_ASSERT_EQUAL_size_t(0, m.internal.total);

    TEST_ASSERT_EQUAL_size_t(0, m.dma.free);
    TEST_ASSERT_EQUAL_size_t(0, m.spiram.free);

    TEST_ASSERT_EQUAL_size_t(0, m.esp_min_free_heap);
    TEST_ASSERT_EQUAL_size_t(0, m.mem_outstanding_bytes);
    TEST_ASSERT_EQUAL_size_t(0, m.mem_peak_outstanding);
    TEST_ASSERT_EQUAL_UINT32(0, m.mem_alloc_fail);

    TEST_ASSERT_EQUAL_size_t(0, m.rtc_used);
    TEST_ASSERT_EQUAL_size_t(0, m.rtc_total);
    TEST_ASSERT_EQUAL_size_t(0, m.dram_static_bytes);
}

void test_bb_meminfo_format_rejects_null(void)
{
    bb_meminfo_snapshot_t m;
    memset(&m, 0, sizeof(m));
    char buf[128];

    TEST_ASSERT_EQUAL(0, bb_meminfo_format(NULL, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL(0, bb_meminfo_format(&m, NULL, sizeof(buf)));
    TEST_ASSERT_EQUAL(0, bb_meminfo_format(&m, buf, 0));
}

void test_bb_meminfo_format_known_snapshot(void)
{
    bb_meminfo_snapshot_t m;
    memset(&m, 0, sizeof(m));
    m.internal.free               = 111000;
    m.internal.min_ever_free      = 90000;
    m.internal.largest_free_block = 65536;
    m.spiram.free                 = 4000000;
    m.dma.free                    = 32000;
    m.esp_min_free_heap           = 85000;

    char buf[128];
    int n = bb_meminfo_format(&m, buf, sizeof(buf));

    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_EQUAL_STRING(
        "heap_int_free=111000 int_min=90000 int_largest=65536 "
        "spiram_free=4000000 dma_free=32000 esp_min_free=85000",
        buf);
}

void test_bb_meminfo_format_truncates_cleanly(void)
{
    bb_meminfo_snapshot_t m;
    memset(&m, 0, sizeof(m));
    m.internal.free = 12345;

    char buf[8];
    int n = bb_meminfo_format(&m, buf, sizeof(buf));

    // snprintf semantics: n is the untruncated length; buf is still
    // NUL-terminated within the given cap.
    TEST_ASSERT_GREATER_THAN(sizeof(buf) - 1, (size_t)n);
    TEST_ASSERT_EQUAL('\0', buf[sizeof(buf) - 1]);
}

// ---------------------------------------------------------------------------
// bb_memreport — the unified memory report (heap + bss + per-region arena
// watermarks). Tests use small, uniquely-named arenas and always deregister
// what they register so state does not leak across tests (the region
// registry is a file-scope static inside the platform impl, capacity
// BB_MEMREPORT_MAX_REGIONS).
// ---------------------------------------------------------------------------

static uint8_t s_memreport_buf_a[256] __attribute__((aligned(_Alignof(max_align_t))));
static uint8_t s_memreport_buf_b[256] __attribute__((aligned(_Alignof(max_align_t))));

void test_bb_memreport_get_null_out_is_noop(void)
{
    bb_memreport_get(NULL); // must not crash
}

void test_bb_memreport_register_arena_rejects_null(void)
{
    bb_mem_arena_t a = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_mem_arena_init(&a, s_memreport_buf_a, sizeof(s_memreport_buf_a)));

    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_memreport_register_arena(NULL, a));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_memreport_register_arena("t_null", NULL));

    bb_mem_arena_destroy(a);
}

void test_bb_memreport_register_arena_rejects_duplicate(void)
{
    bb_mem_arena_t a = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_mem_arena_init(&a, s_memreport_buf_a, sizeof(s_memreport_buf_a)));

    TEST_ASSERT_EQUAL(BB_OK, bb_memreport_register_arena("t_dup", a));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, bb_memreport_register_arena("t_dup", a));

    bb_memreport_deregister("t_dup");
    bb_mem_arena_destroy(a);
}

void test_bb_memreport_deregister_unknown_is_noop(void)
{
    bb_memreport_deregister("t_never_registered"); // must not crash
    bb_memreport_deregister(NULL);                 // must not crash
}

void test_bb_memreport_get_aggregates_registered_arenas(void)
{
    bb_mem_arena_t a = NULL, b = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_mem_arena_init(&a, s_memreport_buf_a, sizeof(s_memreport_buf_a)));
    TEST_ASSERT_EQUAL(BB_OK, bb_mem_arena_init(&b, s_memreport_buf_b, sizeof(s_memreport_buf_b)));

    size_t a_free_before = bb_mem_arena_free_bytes(a);
    void *p = bb_mem_arena_alloc(a, 32);
    TEST_ASSERT_NOT_NULL(p);

    TEST_ASSERT_EQUAL(BB_OK, bb_memreport_register_arena("t_agg_a", a));
    TEST_ASSERT_EQUAL(BB_OK, bb_memreport_register_arena("t_agg_b", b));

    bb_memreport_snapshot_t snap;
    bb_memreport_get(&snap);

    TEST_ASSERT_EQUAL_UINT16(2, snap.region_count);

    const bb_memreport_region_t *ra = NULL, *rb = NULL;
    for (uint16_t i = 0; i < snap.region_count; i++) {
        if (strcmp(snap.regions[i].name, "t_agg_a") == 0) ra = &snap.regions[i];
        if (strcmp(snap.regions[i].name, "t_agg_b") == 0) rb = &snap.regions[i];
    }
    TEST_ASSERT_NOT_NULL(ra);
    TEST_ASSERT_NOT_NULL(rb);

    TEST_ASSERT_EQUAL_UINT(a_free_before - 32, ra->free_bytes);
    TEST_ASSERT_EQUAL_UINT(32, ra->used_bytes);
    TEST_ASSERT_EQUAL_UINT(32, ra->peak_used_bytes);
    TEST_ASSERT_EQUAL_UINT(1, ra->alloc_count);
    TEST_ASSERT_EQUAL_UINT(0, ra->free_count);
    TEST_ASSERT_EQUAL_UINT(0, ra->alloc_failed);

    TEST_ASSERT_EQUAL_UINT(0, rb->used_bytes);
    TEST_ASSERT_EQUAL_UINT(0, rb->peak_used_bytes);
    TEST_ASSERT_EQUAL_UINT(0, rb->alloc_count);

    bb_memreport_deregister("t_agg_a");
    bb_memreport_deregister("t_agg_b");
    bb_mem_arena_destroy(a);
    bb_mem_arena_destroy(b);
}

void test_bb_memreport_peak_used_bytes_survives_reset(void)
{
    bb_mem_arena_t a = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_mem_arena_init(&a, s_memreport_buf_a, sizeof(s_memreport_buf_a)));
    TEST_ASSERT_EQUAL(BB_OK, bb_memreport_register_arena("t_peak", a));

    void *p1 = bb_mem_arena_alloc(a, 96); // high-water mark
    TEST_ASSERT_NOT_NULL(p1);

    bb_mem_arena_reset(a);

    void *p2 = bb_mem_arena_alloc(a, 16); // smaller post-reset allocation
    TEST_ASSERT_NOT_NULL(p2);

    bb_memreport_snapshot_t snap;
    bb_memreport_get(&snap);

    TEST_ASSERT_EQUAL_UINT16(1, snap.region_count);
    TEST_ASSERT_EQUAL_STRING("t_peak", snap.regions[0].name);
    TEST_ASSERT_EQUAL_UINT(16, snap.regions[0].used_bytes);       // current, post-reset
    TEST_ASSERT_EQUAL_UINT(96, snap.regions[0].peak_used_bytes);  // pre-reset high-water mark

    bb_memreport_deregister("t_peak");
    bb_mem_arena_destroy(a);
}

void test_bb_memreport_format_rejects_null(void)
{
    bb_memreport_snapshot_t snap;
    memset(&snap, 0, sizeof(snap));
    char buf[128];

    TEST_ASSERT_EQUAL(0, bb_memreport_format(NULL, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL(0, bb_memreport_format(&snap, NULL, sizeof(buf)));
    TEST_ASSERT_EQUAL(0, bb_memreport_format(&snap, buf, 0));
}

void test_bb_memreport_format_no_regions_is_heap_plus_bss(void)
{
    bb_memreport_snapshot_t snap;
    memset(&snap, 0, sizeof(snap));
    snap.heap.internal.free   = 111000;
    snap.heap.dram_static_bytes = 4096;

    char buf[192];
    int n = bb_memreport_format(&snap, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);

    char heap_line[128];
    bb_meminfo_format(&snap.heap, heap_line, sizeof(heap_line));

    char expected[192];
    snprintf(expected, sizeof(expected), "%s bss=4096", heap_line);
    TEST_ASSERT_EQUAL_STRING(expected, buf);
}

void test_bb_memreport_format_appends_region_tokens(void)
{
    bb_memreport_snapshot_t snap;
    memset(&snap, 0, sizeof(snap));
    snap.region_count = 1;
    strncpy(snap.regions[0].name, "t_fmt", sizeof(snap.regions[0].name) - 1);
    snap.regions[0].free_bytes      = 100;
    snap.regions[0].peak_used_bytes = 40;
    snap.regions[0].used_bytes      = 20;

    char buf[192];
    int n = bb_memreport_format(&snap, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_NOT_NULL(strstr(buf, " t_fmt=100/40/20"));
}

void test_bb_memreport_format_truncates_cleanly(void)
{
    bb_memreport_snapshot_t snap;
    memset(&snap, 0, sizeof(snap));
    snap.region_count = 1;
    strncpy(snap.regions[0].name, "t_trunc", sizeof(snap.regions[0].name) - 1);
    snap.regions[0].free_bytes = 12345;

    char buf[8];
    int n = bb_memreport_format(&snap, buf, sizeof(buf));

    // snprintf semantics: n is the untruncated length; buf is still
    // NUL-terminated within the given cap.
    TEST_ASSERT_GREATER_THAN(sizeof(buf) - 1, (size_t)n);
    TEST_ASSERT_EQUAL('\0', buf[sizeof(buf) - 1]);
}
