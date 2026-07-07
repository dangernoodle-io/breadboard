#include "bb_board.h"
#include "bb_meminfo.h"
#include "bb_str.h"

#include <string.h>

#ifdef BB_BOARD_TESTING
#include "bb_board_test.h"
static bool s_test_ota_validated = false;

void bb_board_test_set_ota_validated(bool validated)
{
    s_test_ota_validated = validated;
}
#endif /* BB_BOARD_TESTING */

bb_err_t bb_board_get_info(bb_board_info_t *out)
{
    if (!out) return BB_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));

    bb_strlcpy(out->board, "host", sizeof(out->board));
    bb_strlcpy(out->project_name, "host", sizeof(out->project_name));
    bb_strlcpy(out->version, "0.0.0-host", sizeof(out->version));
    bb_strlcpy(out->chip_model, "host", sizeof(out->chip_model));
    bb_strlcpy(out->idf_version, "0.0.0-host", sizeof(out->idf_version));
    out->cores = 1;
#ifdef BB_BOARD_TESTING
    out->ota_validated = s_test_ota_validated;
#endif

    return BB_OK;
}

uint32_t bb_board_get_free_heap(void)
{
    return 0;
}

uint32_t bb_board_get_total_heap(void)
{
    return 0;
}

uint32_t bb_board_get_flash_size(void)
{
    return 0;
}

uint32_t bb_board_get_app_size(void)
{
    return 0;
}

uint8_t bb_board_get_cores(void)
{
    return 1;
}

bb_err_t bb_board_get_chip_model(char *out, size_t out_size)
{
    if (!out || out_size == 0) return BB_ERR_INVALID_ARG;
    bb_strlcpy(out, "host", out_size);
    return BB_OK;
}

bb_err_t bb_board_get_mac(char *out, size_t out_size)
{
    if (!out || out_size == 0) return BB_ERR_INVALID_ARG;
    out[0] = '\0';
    return BB_OK;
}

bb_err_t bb_board_get_idf_version(char *out, size_t out_size)
{
    if (!out || out_size == 0) return BB_ERR_INVALID_ARG;
    bb_strlcpy(out, "0.0.0-host", out_size);
    return BB_OK;
}

bb_err_t bb_board_get_reset_reason(char *out, size_t out_size)
{
    if (!out || out_size == 0) return BB_ERR_INVALID_ARG;
    bb_strlcpy(out, "power-on", out_size);
    return BB_OK;
}

// bb_board_heap_* / bb_board_psram_* / bb_board_rtc_* / bb_board_dram_static_bytes
// delegate to bb_meminfo, matching the espidf backend's delegation (SSOT,
// KB #698/#699/#693). bb_meminfo's host stub zeros every field, so behavior
// is unchanged.
size_t bb_board_heap_free_total(void)
{
    bb_meminfo_snapshot_t m;
    bb_meminfo_get(&m);
    return m.default_region.free;
}

size_t bb_board_heap_free_internal(void)
{
    bb_meminfo_snapshot_t m;
    bb_meminfo_get(&m);
    return m.internal.free;
}

size_t bb_board_heap_minimum_ever(void)
{
    bb_meminfo_snapshot_t m;
    bb_meminfo_get(&m);
    return m.default_region.min_ever_free;
}

size_t bb_board_heap_internal_minimum_ever(void)
{
    bb_meminfo_snapshot_t m;
    bb_meminfo_get(&m);
    return m.internal.min_ever_free;
}

size_t bb_board_heap_largest_free_block(void)
{
    bb_meminfo_snapshot_t m;
    bb_meminfo_get(&m);
    return m.default_region.largest_free_block;
}

size_t bb_board_heap_internal_largest_free_block(void)
{
    bb_meminfo_snapshot_t m;
    bb_meminfo_get(&m);
    return m.internal.largest_free_block;
}

uint32_t bb_board_chip_revision(void)
{
    return 0;
}

uint32_t bb_board_cpu_freq_mhz(void)
{
    return 0;
}

size_t bb_board_heap_internal_free(void)
{
    bb_meminfo_snapshot_t m;
    bb_meminfo_get(&m);
    return m.internal.free;
}

size_t bb_board_heap_internal_total(void)
{
    bb_meminfo_snapshot_t m;
    bb_meminfo_get(&m);
    return m.internal.total;
}

size_t bb_board_psram_free(void)
{
    bb_meminfo_snapshot_t m;
    bb_meminfo_get(&m);
    return m.spiram.free;
}

size_t bb_board_psram_total(void)
{
    bb_meminfo_snapshot_t m;
    bb_meminfo_get(&m);
    return m.spiram.total;
}

size_t bb_board_rtc_used(void)
{
    bb_meminfo_snapshot_t m;
    bb_meminfo_get(&m);
    return m.rtc_used;
}

size_t bb_board_rtc_total(void)
{
    bb_meminfo_snapshot_t m;
    bb_meminfo_get(&m);
    return m.rtc_total;
}

size_t bb_board_dram_static_bytes(void)
{
    bb_meminfo_snapshot_t m;
    bb_meminfo_get(&m);
    return m.dram_static_bytes;
}
