#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "bb_core.h"

#ifdef __cplusplus
extern "C" {
#endif

// requires=storage_rtc: the heal/mirror-seed logic in bb_nv_config_init
// (platform/espidf/bb_nv/bb_nv.c) reads and writes the shared "rtc"
// bb_storage backend through bb_settings' mirror accessors, which need
// bb_storage_rtc_register() (provides=storage_rtc, bb_storage_rtc.h) to have
// run first in the same EARLY tier -- same-tier ordering is otherwise
// unspecified, and the heal reading before the backend registers means
// bb_storage_get returns BB_ERR_NOT_FOUND with no crash and no log (the
// RTC backup silently never fires). This edge is gated behind
// CONFIG_BB_NV_CREDS_RTC_BACKUP at runtime (default y), but the marker
// itself is unconditional -- codegen's marker scan has no preprocessor
// awareness (grep-time, see wire_parse.py), same posture as every other
// requires= edge in this codebase (e.g. bb_wifi_autoinit's requires=
// storage_nvs).
// bbtool:init tier=early fn=bb_nv_config_init requires=storage_rtc
bb_err_t bb_nv_config_init(void);

// Register bb_cfg NVS keys with /api/manifest.
// bbtool:init tier=pre_http fn=bb_nv_config_manifest_init
bb_err_t bb_nv_config_manifest_init(void);

/// Thin forwarder to bb_storage_nvs_flash_init() on ESP-IDF (B1-840, bb_nv
/// dissolution epic B1-708) — bb_nv no longer owns NVS partition bring-up.
/// The Arduino backend keeps its own EEPROM-era no-op impl returning BB_OK.
/// Kept for bb_nv's remaining dependents; not a composition-root entry point
/// (the // bbtool:init tier=early marker lives on bb_storage_nvs_register(),
/// which now brings up the partition internally — see bb_storage_nvs.h).
bb_err_t bb_nv_flash_init(void);

#ifndef ESP_PLATFORM
// Test hook: clear the in-memory string store used by host bb_nv_get_str /
// bb_nv_set_str.  Call from setUp() to prevent cross-test leakage.
void bb_nv_host_str_store_reset(void);
#endif

#ifdef ESP_PLATFORM
bool bb_nv_config_is_provisioned(void);
bb_err_t bb_nv_config_set_provisioned(void);
bb_err_t bb_nv_config_clear_provisioned(void);
bb_err_t bb_nv_config_clear_wifi(void);

/// Returns true if the NVS partition was erased this boot (corruption or version
/// mismatch). Always available under ESP_PLATFORM regardless of Kconfig.
bool bb_nv_config_was_erased(void);

/// Returns true if wifi creds were restored from the RTC backup this boot.
/// Always false when CONFIG_BB_NV_CREDS_RTC_BACKUP is disabled.
bool bb_nv_config_creds_restored(void);

bool      bb_nv_config_ota_skip_check(void);
bb_err_t bb_nv_config_set_ota_skip_check(bool skip);

#else
static inline bool bb_nv_config_is_provisioned(void) { return false; }
#endif

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

/// Erase all keys in the given namespace (wrap nvs_erase_all on ESP-IDF;
/// clear all matching entries in the host in-memory store).
/// Returns BB_OK on success; BB_ERR_INVALID_ARG if ns is NULL.
bb_err_t bb_nv_erase_namespace(const char *ns);

/// Returns true if the key exists in the given namespace and its value is
/// non-empty (length > 1 including NUL). Use this to test key presence without
/// reading the full value into a fixed-size probe buffer (which would return
/// false for values longer than the buffer).
bool bb_nv_exists(const char *ns, const char *key);

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
