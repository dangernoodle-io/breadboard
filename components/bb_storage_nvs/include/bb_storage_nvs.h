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
// in platform/espidf/bb_nv/bb_nv.c, keyed directly on an nvs_handle_t. A
// generic, portable multi-key transaction capability now exists alongside
// it in bb_storage.h (bb_storage_txn_begin/set/commit/abort), implemented
// here by nvs_txn_begin/set/commit/abort in bb_storage_nvs.c — the open/set*/
// commit/close-without-commit machinery is the same shape as bb_nv_batch_*,
// generalized to the bb_storage_vtable_t txn group. bb_nv_batch_* itself is
// unchanged and not superseded by this addition; migrating its callers
// (e.g. bb_nv_config_set_wifi_pending) onto the generic txn API is a later,
// separate PR.
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

// ---------------------------------------------------------------------------
// Txn NVS-primitive seam
//
// nvs_txn_begin/set/commit/abort (bb_storage_nvs.c) drive their
// open->set*->commit->close orchestration through this small ops table
// instead of calling nvs_open/nvs_set_*/nvs_commit/nvs_close directly —
// confined to exactly the calls the txn path makes, NOT a general
// abstraction over the get/set/erase/exists surface above. Handles are
// carried as a plain uint32_t (nvs_handle_t's actual underlying type) so
// this seam has no ESP-IDF type in its signature and compiles on host
// without nvs.h. The struct type itself is always available (free — it's
// just a type); the setter and the direct test-entry points below are
// BB_STORAGE_NVS_TESTING-gated, since they exist purely so a host test can
// drive the orchestration against a fake in isolation (the "nvs" backend's
// bb_storage_nvs_register() itself still returns BB_ERR_UNSUPPORTED on
// host — this seam does not change that).
typedef struct {
    bb_err_t (*open)(const char *ns, uint32_t *out_handle);
    bb_err_t (*set_u8)(uint32_t handle, const char *key, uint8_t value);
    bb_err_t (*set_u16)(uint32_t handle, const char *key, uint16_t value);
    bb_err_t (*set_u32)(uint32_t handle, const char *key, uint32_t value);
    bb_err_t (*set_i32)(uint32_t handle, const char *key, int32_t value);
    bb_err_t (*set_str)(uint32_t handle, const char *key, const char *value);
    bb_err_t (*set_blob)(uint32_t handle, const char *key, const void *buf, size_t len);
    bb_err_t (*commit)(uint32_t handle);
    void     (*close)(uint32_t handle);
} bb_storage_nvs_txn_ops_t;

#ifdef BB_STORAGE_NVS_TESTING
// bb_storage.h (bb_storage_txn_t, bb_storage_enc_t) is only needed by the
// testing-only prototypes below -- kept out of the unconditional include
// list above so a production (non-TESTING) build of this component never
// pulls in bb_storage.h's public REQUIRES here (this header stays a plain
// PRIV_REQUIRES bb_storage dependency of bb_storage_nvs.c, not a public one
// every bb_storage_nvs.h consumer -- e.g. bb_nv -- would otherwise need to
// add to ITS OWN REQUIRES too).
#include "bb_storage.h"

// Override the NVS primitives the txn path calls. Pass NULL to clear: on
// host that makes the txn path fail closed (BB_ERR_UNSUPPORTED, never a
// crash) until a test injects a fake; on-device it reverts to the real
// ESP-IDF-backed ops.
void bb_storage_nvs_set_txn_ops_for_test(const bb_storage_nvs_txn_ops_t *ops);

// Direct entry points into the txn orchestration (nvs_txn_begin/set/commit/
// abort), bypassing bb_storage_register_backend()/vtable dispatch — same
// contract as the bb_storage_vtable_t txn_* group (see bb_storage.h).
bb_err_t bb_storage_nvs_txn_begin_for_test(bb_storage_txn_t *txn, const char *ns_or_dir);
bb_err_t bb_storage_nvs_txn_set_for_test(bb_storage_txn_t *txn, const char *key, bb_storage_enc_t enc,
                                          const void *buf, size_t len);
bb_err_t bb_storage_nvs_txn_commit_for_test(bb_storage_txn_t *txn);
bb_err_t bb_storage_nvs_txn_abort_for_test(bb_storage_txn_t *txn);
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
