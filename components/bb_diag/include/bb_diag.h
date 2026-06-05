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
#define BB_DIAG_PANIC_TASK_NAME_MAX  16
#define BB_DIAG_PANIC_BACKTRACE_MAX  16
#define BB_DIAG_PANIC_REASON_MAX     200  /* matches ESP-IDF's example buffer size */
/* SHA256 hex string: up to 64 hex chars + NUL.  Matches the maximum of
 * CONFIG_APP_RETRIEVE_LEN_ELF_SHA (range 8–64, default 9). */
#define BB_DIAG_PANIC_APP_SHA256_MAX 65

typedef struct {
    char     task_name[BB_DIAG_PANIC_TASK_NAME_MAX];
    uint32_t exc_pc;
    uint32_t exc_cause;
    uint32_t bt_count;
    uint32_t bt_addrs[BB_DIAG_PANIC_BACKTRACE_MAX];
    char     panic_reason[BB_DIAG_PANIC_REASON_MAX]; /* full panic reason text from coredump;
                                                        WDT: "Task watchdog got triggered…";
                                                        exception: human-readable cause;
                                                        empty string when not available */
    char     app_sha256[BB_DIAG_PANIC_APP_SHA256_MAX]; /* crashing app ELF SHA256 hex string
                                                          (from esp_core_dump_summary_t.app_elf_sha256);
                                                          empty string when not available */
} bb_diag_panic_summary_t;

/**
 * Returns true if a coredump from the previous boot is decodable.
 * Independent of bb_diag_panic_available — coredump and RTC log mirror
 * are separate capture mechanisms.
 * Host: always returns false.
 */
bool bb_diag_panic_coredump_available(void);

/**
 * Populates `out` with the panic summary (task, exc_pc, exc_cause, backtrace, panic_reason).
 * panic_reason is populated via esp_core_dump_get_panic_reason(); empty string when
 * ESP-IDF returns ESP_ERR_NOT_FOUND (e.g., normal panic without a captured reason).
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

/**
 * Read the raw coredump bytes from the coredump partition into the caller's buffer.
 * `out_len` returns the actual coredump size (not the buffer capacity).
 *
 * Returns:
 *   BB_OK             — coredump read successfully into `buf`; `*out_len` set
 *   BB_ERR_NOT_FOUND  — no valid coredump in flash
 *   BB_ERR_INVALID_ARG — buf NULL, max_len 0, or out_len NULL
 *   BB_ERR_NO_SPACE   — coredump is larger than max_len; *out_len set to required size
 *   BB_ERR_INVALID_STATE — read error
 *
 * Host: always returns BB_ERR_NOT_FOUND.
 */
bb_err_t bb_diag_panic_coredump_read_bytes(uint8_t *buf, size_t max_len, size_t *out_len);

/**
 * Returns the size in bytes of the stored coredump, or 0 if none.
 * Useful to size a buffer before calling bb_diag_panic_coredump_read_bytes.
 */
size_t bb_diag_panic_coredump_size(void);

/**
 * Copy the crashing app's ELF SHA256 hex string into `out` (NUL-terminated).
 * `out` must be at least BB_DIAG_PANIC_APP_SHA256_MAX bytes.
 * Returns BB_OK if a coredump is available and the SHA was populated,
 * BB_ERR_NOT_FOUND if no coredump or SHA is empty,
 * BB_ERR_INVALID_ARG if out or out_size is 0.
 * Host: always returns BB_ERR_NOT_FOUND.
 */
bb_err_t bb_diag_panic_app_sha(char *out, size_t out_size);

/**
 * Erase the coredump image from flash and clear the in-memory summary.
 * Safe to call even when no coredump is present (no-op).
 * MUST NOT clear the panic log record, boots_since counter, or the NVS
 * abnormal-reset counter — only the coredump image and its summary are consumed.
 * Use this after a successful remote pull when ?consume=1 is passed to
 * GET /api/diag/coredump.
 * Host: no-op.
 */
void bb_diag_panic_coredump_erase(void);

/**
 * Returns the persistent count of abnormal resets (panic, task_wdt, int_wdt, wdt, brownout)
 * since this firmware was deployed. Auto-resets to 0 at boot when the running app's SHA
 * fingerprint differs from the one stored at the last counter update (i.e. new flash or OTA).
 * The deploy boot itself is the clean baseline and is NOT counted.
 * Still explicitly clearable via DELETE /api/diag/boot (calls bb_diag_abnormal_reset_count_clear).
 * Survives power cycles via NVS.
 * Host: always returns 0.
 */
uint32_t bb_diag_abnormal_reset_count(void);

/**
 * Resets the abnormal-reset counter to 0 in NVS.
 * Host: no-op.
 */
void bb_diag_abnormal_reset_count_clear(void);

/**
 * Output of bb_diag_reset_decision(): the new counter value to persist and
 * whether the app fingerprint should be stored.
 */
typedef struct {
    uint32_t new_count; /**< counter value to write back to NVS */
    bool     store_fp;  /**< true when running_fp must be written to NVS */
} bb_diag_reset_result_t;

/**
 * Pure decision function for the abnormal-reset counter update.
 * Contains no NVS or ESP-IDF calls — fully host-testable.
 *
 * @param stored_fp   Fingerprint stored in NVS from the last counter update.
 *                    0 means "never stored" (first boot after flash/factory erase).
 * @param running_fp  Fingerprint of the currently running firmware (derived from
 *                    the app ELF SHA256).
 * @param stored_count Counter value read from NVS.
 * @param is_abnormal  True when the reset reason is panic / watchdog / brownout.
 *
 * Logic:
 *   - stored_fp == 0 OR stored_fp != running_fp (new firmware): new_count = 0,
 *     store_fp = true; the deploy boot is the clean baseline, not incremented.
 *   - stored_fp == running_fp (same build): new_count = stored_count + (is_abnormal ? 1 : 0),
 *     store_fp = false (fp unchanged).
 */
bb_diag_reset_result_t bb_diag_reset_decision(uint32_t stored_fp, uint32_t running_fp,
                                               uint32_t stored_count, bool is_abnormal);
