// test_snap_desc — byte-fidelity + assembler coverage for the source-owned,
// format-agnostic heap and system snapshot descriptors (bb_meminfo,
// bb_system, B1-767-family). ADDITIVE only -- neither descriptor is
// consumed by any serializer/route yet; these are the only callers today.

#include "unity.h"

#include "bb_serialize_json.h"

#include "../../components/bb_meminfo/bb_meminfo_heap_snap.h"
#include "../../components/bb_system/bb_system_snap.h"

#include <string.h>

static void render_eq(const bb_serialize_desc_t *d, const void *snap, const char *golden)
{
    char   buf[512];
    size_t n = 0;

    TEST_ASSERT_EQUAL_INT(BB_OK, bb_serialize_json_render(d, snap, buf, sizeof buf, &n));
    TEST_ASSERT_EQUAL_STRING(golden, buf);
    TEST_ASSERT_EQUAL_UINT(strlen(golden), n);
}

// ---------------------------------------------------------------------------
// meminfo
// ---------------------------------------------------------------------------

static void fill_meminfo_region(bb_meminfo_heap_snap_region_t *r, uint64_t base)
{
    r->free               = base;
    r->min_ever_free      = base + 1;
    r->largest_free_block = base + 2;
    r->total              = base + 3;
}

void test_snap_desc_meminfo_render_populated(void)
{
    bb_meminfo_heap_snap_t snap;
    memset(&snap, 0, sizeof(snap));

    fill_meminfo_region(&snap.default_region, 100);
    fill_meminfo_region(&snap.internal, 200);
    fill_meminfo_region(&snap.dma, 300);
    fill_meminfo_region(&snap.spiram, 400);
    snap.esp_min_free_heap      = 500;
    snap.mem_outstanding_bytes  = 600;
    snap.mem_peak_outstanding   = 700;
    snap.mem_alloc_fail         = 800;
    snap.rtc_used               = 900;
    snap.rtc_total              = 1000;
    snap.dram_static_bytes      = 1100;

    render_eq(&bb_meminfo_heap_snap_desc, &snap,
              "{\"default\":{\"free\":100,\"min_ever_free\":101,\"largest_free_block\":102,\"total\":103},"
              "\"internal\":{\"free\":200,\"min_ever_free\":201,\"largest_free_block\":202,\"total\":203},"
              "\"dma\":{\"free\":300,\"min_ever_free\":301,\"largest_free_block\":302,\"total\":303},"
              "\"spiram\":{\"free\":400,\"min_ever_free\":401,\"largest_free_block\":402,\"total\":403},"
              "\"esp_min_free_heap\":500,"
              "\"mem_outstanding_bytes\":600,\"mem_peak_outstanding\":700,\"mem_alloc_fail\":800,"
              "\"rtc_used\":900,\"rtc_total\":1000,\"dram_static_bytes\":1100}");
}

void test_snap_desc_meminfo_render_no_space_all_or_nothing(void)
{
    bb_meminfo_heap_snap_t snap;
    memset(&snap, 0, sizeof(snap));
    fill_meminfo_region(&snap.default_region, 1);

    char   buf[8]; // far too small for the full document
    size_t n = 123;
    memset(buf, 0x7F, sizeof(buf));

    TEST_ASSERT_EQUAL_INT(BB_ERR_NO_SPACE,
                           bb_serialize_json_render(&bb_meminfo_heap_snap_desc, &snap, buf, sizeof buf, &n));
    TEST_ASSERT_EQUAL_UINT(0, n);
    TEST_ASSERT_EQUAL_CHAR('\0', buf[0]);
}

void test_snap_desc_meminfo_snap_fill_rejects_null(void)
{
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, bb_meminfo_heap_snap_fill(NULL));
}

void test_snap_desc_meminfo_snap_fill_host_zeroes(void)
{
    bb_meminfo_heap_snap_t snap;
    memset(&snap, 0xAA, sizeof(snap));

    TEST_ASSERT_EQUAL_INT(BB_OK, bb_meminfo_heap_snap_fill(&snap));

    // bb_meminfo_get() zero-fills on host -- bb_meminfo_heap_snap_fill() widens
    // that zeroed snapshot field-for-field, so every field renders 0.
    render_eq(&bb_meminfo_heap_snap_desc, &snap,
              "{\"default\":{\"free\":0,\"min_ever_free\":0,\"largest_free_block\":0,\"total\":0},"
              "\"internal\":{\"free\":0,\"min_ever_free\":0,\"largest_free_block\":0,\"total\":0},"
              "\"dma\":{\"free\":0,\"min_ever_free\":0,\"largest_free_block\":0,\"total\":0},"
              "\"spiram\":{\"free\":0,\"min_ever_free\":0,\"largest_free_block\":0,\"total\":0},"
              "\"esp_min_free_heap\":0,"
              "\"mem_outstanding_bytes\":0,\"mem_peak_outstanding\":0,\"mem_alloc_fail\":0,"
              "\"rtc_used\":0,\"rtc_total\":0,\"dram_static_bytes\":0}");
}

// ---------------------------------------------------------------------------
// system
// ---------------------------------------------------------------------------

void test_snap_desc_system_render_fixed_fixture(void)
{
    bb_system_snap_t snap;
    memset(&snap, 0, sizeof(snap));
    strncpy(snap.version, "1.2.3", sizeof(snap.version) - 1);
    strncpy(snap.project_name, "widget-fw", sizeof(snap.project_name) - 1);
    strncpy(snap.idf_version, "v5.1.2", sizeof(snap.idf_version) - 1);
    strncpy(snap.reset_reason, "power-on", sizeof(snap.reset_reason) - 1);
    snap.boot_count = 3;
    strncpy(snap.build_date, "Jan  1 2026", sizeof(snap.build_date) - 1);
    strncpy(snap.build_time, "12:34:56", sizeof(snap.build_time) - 1);

    render_eq(&bb_system_snap_desc, &snap,
              "{\"version\":\"1.2.3\",\"project_name\":\"widget-fw\",\"idf_version\":\"v5.1.2\","
              "\"reset_reason\":\"power-on\",\"boot_count\":3,"
              "\"build_date\":\"Jan  1 2026\",\"build_time\":\"12:34:56\"}");
}

void test_snap_desc_system_render_no_space_all_or_nothing(void)
{
    bb_system_snap_t snap;
    memset(&snap, 0, sizeof(snap));
    strncpy(snap.version, "1.2.3", sizeof(snap.version) - 1);

    char   buf[8]; // far too small for the full document
    size_t n = 123;
    memset(buf, 0x7F, sizeof(buf));

    TEST_ASSERT_EQUAL_INT(BB_ERR_NO_SPACE,
                           bb_serialize_json_render(&bb_system_snap_desc, &snap, buf, sizeof buf, &n));
    TEST_ASSERT_EQUAL_UINT(0, n);
    TEST_ASSERT_EQUAL_CHAR('\0', buf[0]);
}

void test_snap_desc_system_snap_fill_rejects_null(void)
{
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, bb_system_snap_fill(NULL));
}

void test_snap_desc_system_snap_fill_host_defaults(void)
{
    bb_system_snap_t snap;
    memset(&snap, 0xAA, sizeof(snap));

    TEST_ASSERT_EQUAL_INT(BB_OK, bb_system_snap_fill(&snap));

    // Deterministic host fallbacks (bb_system_host.c / bb_system.h doc).
    TEST_ASSERT_EQUAL_STRING("0.0.0-host", snap.version);
    TEST_ASSERT_EQUAL_STRING("host", snap.project_name);
    TEST_ASSERT_EQUAL_STRING("0.0.0-host", snap.idf_version);
    TEST_ASSERT_EQUAL_STRING("power-on", snap.reset_reason);
    TEST_ASSERT_EQUAL_UINT64(0, snap.boot_count);

    // build_date/build_time are compiler __DATE__/__TIME__ -- not
    // deterministic across builds, but always non-empty and within bound.
    TEST_ASSERT_TRUE(strlen(snap.build_date) > 0);
    TEST_ASSERT_TRUE(strlen(snap.build_date) < sizeof(snap.build_date));
    TEST_ASSERT_TRUE(strlen(snap.build_time) > 0);
    TEST_ASSERT_TRUE(strlen(snap.build_time) < sizeof(snap.build_time));
}
