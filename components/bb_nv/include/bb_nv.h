#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "bb_core.h"

#ifdef __cplusplus
extern "C" {
#endif

bb_err_t bb_nv_config_init(void);

// Register bb_cfg NVS keys with /api/manifest. Called automatically at
// PRE_HTTP tier when CONFIG_BB_NV_CONFIG_MANIFEST_AUTOREGISTER=y (default).
// Consumers may also call it directly before bb_registry_init() if needed.
bb_err_t bb_nv_config_manifest_init(void);

/// Initialize the NV flash partition. Handles the
/// ESP_ERR_NVS_NO_FREE_PAGES / NEW_VERSION_FOUND erase-and-retry case.
/// Idempotent — safe to call multiple times.
bb_err_t bb_nv_flash_init(void);

const char *bb_nv_config_wifi_ssid(void);
const char *bb_nv_config_wifi_pass(void);
const char *bb_nv_config_hostname(void);
bool bb_nv_config_display_enabled(void);
bool bb_nv_config_mdns_enabled(void);
bool bb_nv_config_update_check_enabled(void);

#ifndef ESP_PLATFORM
// Test hook: force the next bb_nv_config_set_update_check_enabled call to fail.
// Reset to false after the test.
void bb_nv_config_host_force_set_update_check_fail(bool fail);
#endif

#ifdef ESP_PLATFORM
bool bb_nv_config_is_provisioned(void);
bb_err_t bb_nv_config_set_provisioned(void);
bb_err_t bb_nv_config_clear_provisioned(void);
bb_err_t bb_nv_config_clear_wifi(void);

#define BB_NV_CONFIG_BOOT_FAIL_THRESHOLD 3
uint8_t   bb_nv_config_boot_count(void);
bb_err_t bb_nv_config_increment_boot_count(void);
bb_err_t bb_nv_config_reset_boot_count(void);

bool      bb_nv_config_ota_skip_check(void);
bb_err_t bb_nv_config_set_ota_skip_check(bool skip);

bb_err_t bb_nv_config_set_wifi(const char *ssid, const char *pass);
bb_err_t bb_nv_config_set_hostname(const char *hostname);
bb_err_t bb_nv_config_set_display_enabled(bool en);
bb_err_t bb_nv_config_set_mdns_enabled(bool en);
#else
static inline bool bb_nv_config_is_provisioned(void) { return false; }
// Host: set_hostname implemented in platform/espidf/bb_nv/bb_nv.c for host builds
bb_err_t bb_nv_config_set_hostname(const char *hostname);
#endif

// Available on all platforms: in-memory on host, NVS-persisted on ESP-IDF.
bb_err_t bb_nv_config_set_update_check_enabled(bool en);

// All bb_nv_set_u* variants erase-on-type-mismatch: if an existing NVS entry
// under the same key has a different integer width, the entry is erased and
// the set retried, so cross-version type changes are transparent.
//
// All bb_nv_get_u* variants treat type mismatch the same as a missing key —
// *out is set to fallback and BB_OK is returned (a WARN is logged so drift
// is visible). Consumers never need to care about residual NVS shapes.
bb_err_t bb_nv_set_u8 (const char *ns, const char *key, uint8_t  value);
bb_err_t bb_nv_set_u16(const char *ns, const char *key, uint16_t value);
bb_err_t bb_nv_set_u32(const char *ns, const char *key, uint32_t value);
bb_err_t bb_nv_set_str(const char *ns, const char *key, const char *value);
bb_err_t bb_nv_get_u8 (const char *ns, const char *key, uint8_t  *out, uint8_t  fallback);
bb_err_t bb_nv_get_u16(const char *ns, const char *key, uint16_t *out, uint16_t fallback);
bb_err_t bb_nv_get_u32(const char *ns, const char *key, uint32_t *out, uint32_t fallback);
bb_err_t bb_nv_get_str(const char *ns, const char *key, char *buf, size_t len, const char *fallback);
bb_err_t bb_nv_erase  (const char *ns, const char *key);

/* ---------------------------------------------------------------------------
 * Batched setters
 *
 * Collapse multiple set_* operations into one open + commit + close cycle.
 * On ESP-IDF that reduces N flash transactions to 1, cutting ~(N-1) × commit
 * worth of SPI-bus contention — important on the share-accept hot path where
 * the per-key API can stall higher-priority tasks waiting on flash. On
 * Arduino each set is forwarded to the per-key API (no batching benefit but
 * API parity preserved). On host the calls are no-ops that validate args.
 *
 *   bb_nv_batch_t batch;
 *   bb_err_t err = bb_nv_batch_begin(&batch, "ns");
 *   if (err != BB_OK) return err;
 *   bb_nv_batch_set_u32(&batch, "k1", v1);
 *   bb_nv_batch_set_u32(&batch, "k2", v2);
 *   err = bb_nv_batch_commit(&batch);   // commits + closes (always)
 *
 * Errors are sticky: the first set_* failure poisons the batch and is
 * returned by bb_nv_batch_commit. The underlying handle is closed regardless,
 * so the batch is single-use.
 *
 * The struct contents are backend-private; never inspect or modify them.
 * --------------------------------------------------------------------------- */
typedef struct bb_nv_batch_s {
    uintptr_t _impl;       /* ESP-IDF: nvs_handle_t; other backends: private */
    int       _err;        /* sticky first error, BB_OK initially */
    uint8_t   _open;       /* nonzero between begin and commit */
    char      _ns[16];     /* namespace copy (NVS max is 15+null) */
} bb_nv_batch_t;

bb_err_t bb_nv_batch_begin (bb_nv_batch_t *batch, const char *ns);
bb_err_t bb_nv_batch_set_u8 (bb_nv_batch_t *batch, const char *key, uint8_t  value);
bb_err_t bb_nv_batch_set_u16(bb_nv_batch_t *batch, const char *key, uint16_t value);
bb_err_t bb_nv_batch_set_u32(bb_nv_batch_t *batch, const char *key, uint32_t value);
bb_err_t bb_nv_batch_set_str(bb_nv_batch_t *batch, const char *key, const char *value);
bb_err_t bb_nv_batch_commit(bb_nv_batch_t *batch);

#ifdef __cplusplus
}
#endif
