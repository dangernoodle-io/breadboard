#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bb_core.h"

#ifdef __cplusplus
extern "C" {
#endif

// Runtime info snapshot. Fields that the active backend cannot populate
// are zeroed / empty. Intended for GET /api/board.
typedef struct {
    char board[32];         // FIRMWARE_BOARD value, or empty
    char project_name[32];
    char version[32];
    char idf_version[32];
    char build_date[16];
    char build_time[16];
    char chip_model[16];
    uint8_t cores;
    char mac[18];           // lowercase colon-separated, or empty
    uint32_t flash_size;    // bytes, 0 if unknown
    uint32_t total_heap;
    uint32_t free_heap;
    uint32_t app_size;      // running partition size, 0 if no partitions
    char reset_reason[16];  // "power-on", "software", "panic", ...
    bool ota_validated;     // true unless running slot is PENDING_VERIFY
} bb_board_info_t;

// Populate out with a runtime snapshot. Returns BB_OK on success.
bb_err_t bb_board_get_info(bb_board_info_t *out);

// Discrete accessors. Numeric getters return 0 when the backend cannot
// determine the value; string getters fill out with an empty string
// and return BB_OK. All string getters expect out_size >= 1.
uint32_t bb_board_get_free_heap (void);
uint32_t bb_board_get_total_heap(void);
uint32_t bb_board_get_flash_size(void);
uint32_t bb_board_get_app_size  (void);
uint8_t  bb_board_get_cores     (void);

bb_err_t bb_board_get_chip_model   (char *out, size_t out_size);
bb_err_t bb_board_get_mac          (char *out, size_t out_size);
bb_err_t bb_board_get_idf_version  (char *out, size_t out_size);
bb_err_t bb_board_get_reset_reason (char *out, size_t out_size);

// PSRAM-inclusive (MALLOC_CAP_DEFAULT). Differs from bb_board_get_free_heap
// which is INTERNAL only.
size_t   bb_board_heap_free_total(void);
size_t   bb_board_heap_free_internal(void);
size_t   bb_board_heap_minimum_ever(void);
size_t   bb_board_heap_largest_free_block(void);
uint32_t bb_board_chip_revision(void);
uint32_t bb_board_cpu_freq_mhz(void);

// Internal-only heap (MALLOC_CAP_INTERNAL). 0 if unavailable.
size_t   bb_board_heap_internal_free(void);
size_t   bb_board_heap_internal_total(void);
size_t   bb_board_heap_internal_largest_free_block(void);
size_t   bb_board_heap_internal_minimum_ever(void); // MALLOC_CAP_INTERNAL watermark

// PSRAM heap (MALLOC_CAP_SPIRAM). Both 0 on boards with no PSRAM.
size_t   bb_board_psram_free(void);
size_t   bb_board_psram_total(void);

// RTC slow memory region. Not a heap — statically partitioned.
// used = bytes consumed by static RTC sections (.rtc.data, .rtc.bss,
//         .rtc_noinit, .rtc_force_slow). total = full RTC slow region size.
// Both 0 on platforms where RTC slow memory is not supported.
size_t   bb_board_rtc_used(void);
size_t   bb_board_rtc_total(void);

// Internal DRAM static usage (.data + .bss sections combined).
// Returns the total bytes occupied by statically-placed symbols in internal
// SRAM at link time — the BSS cost of any static reservations is visible here.
// Returns 0 on platforms where the linker symbols are not available (host).
size_t   bb_board_dram_static_bytes(void);

// ---------------------------------------------------------------------------
// Heap state — moved from bb_net_health (net_health teardown PR-C). Pure,
// host-testable, no ESP-IDF dependency. bb_net_health's evaluator classifies
// bb_board_heap_free_total() each cycle and stores the result here.
// ---------------------------------------------------------------------------

// Heap-threshold thresholds (compile-time overridable). On ESP-IDF, Kconfig
// generates CONFIG_BB_BOARD_* symbols; bridge them here so menuconfig
// changes take effect. On the host build there is no sdkconfig, so we fall
// straight through to the numeric fallbacks.
#ifdef ESP_PLATFORM
#  ifdef CONFIG_BB_BOARD_HEAP_LOW_BYTES
#    define BB_BOARD_HEAP_LOW_BYTES CONFIG_BB_BOARD_HEAP_LOW_BYTES
#  endif
#endif
#ifndef BB_BOARD_HEAP_LOW_BYTES
#define BB_BOARD_HEAP_LOW_BYTES      40000  // free heap bytes below which → LOW
#endif

#ifdef ESP_PLATFORM
#  ifdef CONFIG_BB_BOARD_HEAP_CRITICAL_BYTES
#    define BB_BOARD_HEAP_CRITICAL_BYTES CONFIG_BB_BOARD_HEAP_CRITICAL_BYTES
#  endif
#endif
#ifndef BB_BOARD_HEAP_CRITICAL_BYTES
#define BB_BOARD_HEAP_CRITICAL_BYTES 20000  // free heap bytes below which → CRITICAL
#endif

/**
 * Coarse heap health bucket.  Zero-init is BB_BOARD_HEAP_STATE_OK so host
 * stubs and uninitialised-state callers always get a sane default.
 */
typedef enum {
    BB_BOARD_HEAP_STATE_OK       = 0,
    BB_BOARD_HEAP_STATE_LOW      = 1,
    BB_BOARD_HEAP_STATE_CRITICAL = 2,
} bb_board_heap_state_t;

/**
 * Pure heap classifier: maps total free heap bytes to a bb_board_heap_state_t
 * bucket against the BB_BOARD_HEAP_LOW_BYTES / BB_BOARD_HEAP_CRITICAL_BYTES
 * thresholds.  No side-effects; host-testable.
 */
bb_board_heap_state_t bb_board_classify_heap(size_t free_bytes);

/**
 * Return the latest heap state computed by the evaluator.
 * Thread-safe: reads a module-static set by the evaluator.
 * Returns BB_BOARD_HEAP_STATE_OK on host (evaluator never runs).
 */
bb_board_heap_state_t bb_board_heap_state(void);

/**
 * Return a static string for a bb_board_heap_state_t value.
 * "ok", "low", or "critical".  Never returns NULL.
 */
const char *bb_board_heap_state_str(bb_board_heap_state_t state);

#ifdef ESP_PLATFORM
#endif

#ifdef __cplusplus
}
#endif
