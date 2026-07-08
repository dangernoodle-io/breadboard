#pragma once

// bb_storage_nvs — ESP-IDF NVS backend for bb_storage.
//
// Implements the bb_storage_vtable_t contract (get/set/erase/exists) over
// ESP-IDF's NVS API and registers itself under the backend name "nvs" via
// bb_storage_nvs_register() — composition-only, no self-registration (see
// bb_storage.h's file header and CLAUDE.md's DI legacy fence). Through the
// generic bb_storage_get/set/erase/exists facade, addr->ns_or_dir maps to
// the NVS namespace (<=15 chars) and addr->key maps to the NVS key; values
// are stored/read as raw blobs (nvs_set_blob/nvs_get_blob).
//
// bb_nv's typed accessors (bb_nv_get_u8/set_u8/set_u16/set_u32/get_str/
// set_str/erase/erase_namespace/exists) are thin forwarders to the typed
// functions declared below, NOT to the generic blob-shaped vtable above.
// NVS's typed entries (U8/U16/U32/STR) are not representable as a single
// blob shape without migrating every existing on-flash entry — so this
// component keeps the original typed nvs_get_u8/nvs_set_str/etc access
// path (moved here verbatim from platform/espidf/bb_nv/bb_nv.c) under new
// names, preserving on-flash format byte-for-byte. bb_storage_nvs_register()
// separately wires the generic blob vtable for callers that go through the
// portable bb_storage_get/set/erase/exists facade with backend="nvs" (e.g.
// a future generic-storage consumer) — existing bb_nv_* consumers do not
// need it registered at all.
//
// bb_nv_batch_* stays out of scope for this component (NVS-backend-private,
// deliberately not part of the generic facade) — its implementation stays
// in platform/espidf/bb_nv/bb_nv.c, keyed directly on an nvs_handle_t.
//
// On host builds (no ESP_PLATFORM) every function below is a no-op stub
// that validates arguments and returns BB_ERR_UNSUPPORTED (bool functions
// return false) — bb_nv's host build never routes through this component
// (it keeps its existing in-memory string-store stub), so these stubs
// exist purely so this header/component compile on host per breadboard's
// portability rule.

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "bb_core.h"

#ifdef __cplusplus
extern "C" {
#endif

// Register the "nvs" backend with bb_storage: generic blob get/set/erase/
// exists via addr->ns_or_dir as NVS namespace, addr->key as NVS key.
// Composition-only — call explicitly from application composition code or
// test setup. Idempotent-registration policy is bb_storage's (first
// registration wins; a duplicate call returns BB_ERR_INVALID_STATE).
bb_err_t bb_storage_nvs_register(void);

// Typed single-key accessors — same contract as the bb_nv_* functions of
// the same shape (see bb_nv.h) that forward to these. All
// bb_storage_nvs_set_u* variants erase-on-type-mismatch: if an existing NVS
// entry under the same key has a different integer width, the entry is
// erased and the set retried. All bb_storage_nvs_get_u* variants treat type
// mismatch the same as a missing key — *out is set to fallback and BB_OK is
// returned (a WARN is logged so drift is visible).
bb_err_t bb_storage_nvs_set_u8 (const char *ns, const char *key, uint8_t  value);
bb_err_t bb_storage_nvs_set_u16(const char *ns, const char *key, uint16_t value);
bb_err_t bb_storage_nvs_set_u32(const char *ns, const char *key, uint32_t value);
bb_err_t bb_storage_nvs_set_i32(const char *ns, const char *key, int32_t  value);
bb_err_t bb_storage_nvs_set_str(const char *ns, const char *key, const char *value);
bb_err_t bb_storage_nvs_get_u8 (const char *ns, const char *key, uint8_t  *out, uint8_t  fallback);
bb_err_t bb_storage_nvs_get_u16(const char *ns, const char *key, uint16_t *out, uint16_t fallback);
bb_err_t bb_storage_nvs_get_u32(const char *ns, const char *key, uint32_t *out, uint32_t fallback);
bb_err_t bb_storage_nvs_get_i32(const char *ns, const char *key, int32_t  *out, int32_t  fallback);
bb_err_t bb_storage_nvs_get_str(const char *ns, const char *key, char *buf, size_t len, const char *fallback);

// Erase a single key. Idempotent — a missing key is not an error.
bb_err_t bb_storage_nvs_erase(const char *ns, const char *key);

// Erase all keys in the given namespace. A namespace that does not exist
// yet is treated as already-clean (BB_OK), not an error.
bb_err_t bb_storage_nvs_erase_namespace(const char *ns);

// Returns true if the key exists in the given namespace and its stored
// value is non-empty (length > 1 including NUL).
bool bb_storage_nvs_exists(const char *ns, const char *key);

#ifdef __cplusplus
}
#endif
