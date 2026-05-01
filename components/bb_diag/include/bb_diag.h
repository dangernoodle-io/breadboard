#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "bb_core.h"

/**
 * Returns true if a panic log is available from the previous boot's abnormal shutdown.
 * ESP-IDF: true if previous reset was abnormal (panic, watchdog, etc.) and log was captured.
 * Host: always returns false.
 */
bool bb_diag_panic_available(void);

/**
 * Retrieves the captured panic log tail into the caller-supplied buffer.
 * *len_inout: in=buffer capacity, out=bytes written (excluding NUL).
 * ESP-IDF: Returns BB_OK and populates out if panic log is available and fits.
 * ESP-IDF: Returns BB_ERR_NOT_FOUND if no panic log or previous boot was clean.
 * Host: always returns BB_ERR_NOT_FOUND.
 */
bb_err_t bb_diag_panic_get(char *out, size_t *len_inout);

/**
 * Clear the panic record after it has been read or acknowledged.
 * Safe to call even if no panic log is available.
 * When CONFIG_BB_DIAG_PANIC_COREDUMP=y, also erases the coredump image from flash.
 */
void bb_diag_panic_clear(void);

/**
 * Structured summary of the previous boot's panic, populated from ESP-IDF coredump.
 * Requires CONFIG_BB_DIAG_PANIC_COREDUMP=y plus a "coredump" partition in partitions.csv.
 */
#define BB_DIAG_PANIC_TASK_NAME_MAX 16
#define BB_DIAG_PANIC_BACKTRACE_MAX 16

typedef struct {
    char     task_name[BB_DIAG_PANIC_TASK_NAME_MAX];
    uint32_t exc_pc;
    uint32_t exc_cause;
    uint32_t bt_count;
    uint32_t bt_addrs[BB_DIAG_PANIC_BACKTRACE_MAX];
} bb_diag_panic_summary_t;

/**
 * Returns true if a coredump from the previous boot is decodable.
 * Independent of bb_diag_panic_available — coredump and RTC log mirror
 * are separate capture mechanisms.
 * Host: always returns false.
 */
bool bb_diag_panic_coredump_available(void);

/**
 * Populates `out` with the panic summary (task, exc_pc, exc_cause, backtrace).
 * Returns BB_OK on success, BB_ERR_NOT_FOUND if no coredump,
 * BB_ERR_INVALID_ARG if out=NULL.
 * Host: always returns BB_ERR_NOT_FOUND.
 */
bb_err_t bb_diag_panic_coredump_get(bb_diag_panic_summary_t *out);

/**
 * Returns the number of boots since the most recent panic was captured.
 *   0 = current boot was the post-panic boot (panic just happened)
 *   N = N clean boots have happened since the panic was captured
 * Returns 0 when no panic is available, or when a power cycle reset the
 * RTC-backed counter (the coredump on flash may still be present, but the
 * counter cannot distinguish a fresh capture from a post-power-cycle boot).
 * Useful as a freshness signal for monitoring callers.
 * Host: always returns 0.
 */
uint32_t bb_diag_panic_boots_since(void);
