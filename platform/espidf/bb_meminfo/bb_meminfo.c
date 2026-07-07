#include "bb_meminfo.h"
#include "bb_mem.h"

#include <stdint.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_system.h"
#include "soc/soc.h"

static void fill_region(bb_meminfo_region_t *r, uint32_t caps, uint32_t largest_caps)
{
    r->free               = heap_caps_get_free_size(caps);
    r->min_ever_free      = heap_caps_get_minimum_free_size(caps);
    r->largest_free_block = heap_caps_get_largest_free_block(largest_caps);
    r->total              = heap_caps_get_total_size(caps);
}

bb_err_t bb_meminfo_get(bb_meminfo_snapshot_t *out)
{
    if (!out) return BB_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));

    fill_region(&out->default_region, MALLOC_CAP_DEFAULT, MALLOC_CAP_DEFAULT);
    fill_region(&out->internal, MALLOC_CAP_INTERNAL,
                MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    fill_region(&out->dma, MALLOC_CAP_DMA, MALLOC_CAP_DMA);
    fill_region(&out->spiram, MALLOC_CAP_SPIRAM, MALLOC_CAP_SPIRAM);

    out->esp_min_free_heap = esp_get_minimum_free_heap_size();

    bb_mem_stats_t stats;
    bb_mem_get_stats(&stats);
    out->mem_outstanding_bytes = stats.outstanding_bytes;
    out->mem_peak_outstanding  = stats.peak_outstanding;
    out->mem_alloc_fail        = stats.alloc_fail;

    /*
     * RTC slow memory + internal DRAM static accounting — mirrors
     * bb_board_rtc_used/_total/_dram_static_bytes exactly (see those
     * functions' comments in platform/espidf/bb_board/bb_board.c for the
     * full rationale); duplicated here rather than called into so bb_board
     * can delegate to bb_meminfo without a circular include.
     */
#ifdef SOC_RTC_DATA_LOW
    {
        extern int _rtc_data_start, _rtc_data_end;
        extern int _rtc_bss_start, _rtc_bss_end;
        extern int _rtc_noinit_start, _rtc_noinit_end;
        extern int _rtc_force_slow_start, _rtc_force_slow_end;

        uintptr_t lo = (uintptr_t)&_rtc_data_start;
        uintptr_t hi = (uintptr_t)&_rtc_data_end;

#define BB_RTC_EXTEND(s, e) do { \
    uintptr_t _s = (uintptr_t)&(s); uintptr_t _e = (uintptr_t)&(e); \
    if (_s < lo) lo = _s; \
    if (_e > hi) hi = _e; \
} while (0)

        BB_RTC_EXTEND(_rtc_bss_start,        _rtc_bss_end);
        BB_RTC_EXTEND(_rtc_noinit_start,     _rtc_noinit_end);
        BB_RTC_EXTEND(_rtc_force_slow_start, _rtc_force_slow_end);
#undef BB_RTC_EXTEND

        out->rtc_used  = (hi > lo) ? (size_t)(hi - lo) : 0;
        out->rtc_total = (size_t)(SOC_RTC_DATA_HIGH - SOC_RTC_DATA_LOW);
    }
#endif

    {
        extern int _data_start __attribute__((weak));
        extern int _data_end   __attribute__((weak));
        extern int _bss_start  __attribute__((weak));
        extern int _bss_end    __attribute__((weak));

        size_t dram = 0;
        if (&_data_start && &_data_end &&
            (uintptr_t)&_data_end > (uintptr_t)&_data_start) {
            dram += (size_t)((uintptr_t)&_data_end - (uintptr_t)&_data_start);
        }
        if (&_bss_start && &_bss_end &&
            (uintptr_t)&_bss_end > (uintptr_t)&_bss_start) {
            dram += (size_t)((uintptr_t)&_bss_end - (uintptr_t)&_bss_start);
        }
        out->dram_static_bytes = dram;
    }

    return BB_OK;
}
