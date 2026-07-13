#pragma once

/**
 * @brief bb_config_staged — part of bb_config: a generic staged multi-field
 * write session over bb_config's typed layer, atomic commit via
 * bb_storage's txn group.
 */

// A consumer stages several bb_config_field_t writes (bool/u8/u16/u32/i32/
// str/blob) against ONE backend/namespace, then commits them as a single
// atomic + durable operation, or discards them. This is bb_config_field_t's
// staged-write primitive -- it wraps bb_storage_txn_t (the atomic transport)
// and reuses bb_config's own encode path (bb_config_type_to_enc(),
// bb_config_scalar_width(), the bb_byte_order helpers) so a staged value is
// byte-identical to what bb_config_set_*() would have written directly.
//
// Two layered sticky-error models compose here: bb_storage_txn_t owns its
// OWN sticky error for delegated (txn-level) failures (e.g. BB_ERR_NO_SPACE
// on an oversize value) -- this handle does not duplicate that state. This
// layer adds exactly one more sticky slot, _local_err, for LOCAL precheck
// failures that never reach the txn (mismatched type, cross-namespace field,
// oversize str/blob against max_len) -- first-wins, and once set, every
// subsequent set_* call short-circuits without touching the txn. commit()
// checks _local_err first: if set, it aborts the (possibly still-open, but
// untouched-by-the-failing-call) txn and returns _local_err; otherwise it
// delegates straight to bb_storage_txn_commit(), whose own sticky error (if
// any) naturally propagates.
//
// Homogeneous backend/ns per session: every field staged into one handle
// must share the SAME addr.backend/addr.ns_or_dir set at begin() -- staging
// a field from a different namespace is a caller bug, rejected as
// BB_ERR_INVALID_ARG without touching the txn.
//
// No heap: h is caller-allocated (stack/static) and MUST be zero-initialized
// before bb_config_staged_begin(), matching bb_storage_txn_t's own caller-init
// convention (h->txn is zeroed as part of h's zero-init). Capacity is
// entirely bb_storage's txn caps (BB_STORAGE_TXN_MAX_KEYS/_VALUE_MAX_BYTES/
// _KEY_MAX_BYTES) -- no second buffer here, so no capacity knobs of this
// sub-namespace's own.
//
// Composition-only: no global state, no init function, nothing self-
// registers (see the DI legacy fence in breadboard/CLAUDE.md).
//
// Single-use closed contract: once commit() or discard() has run, the
// session is closed (_closed is set). Any call after that -- another
// commit(), another set_*, or begin() reused without a fresh zero-init --
// returns BB_ERR_INVALID_STATE, EXCEPT discard(), which stays idempotent
// (returns BB_OK) on an already-closed or never-begun handle. This makes
// the closed signal coherent regardless of which path closed the session
// (a clean commit, a local-precheck-poisoned commit, or a discard).

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bb_config.h"
#include "bb_core.h"
#include "bb_storage.h"

#ifdef __cplusplus
extern "C" {
#endif

// Opaque, caller-allocated (stack/static, NO HEAP) staged-write session
// handle. Zero-initialize before bb_config_staged_begin() -- same caller-
// init convention as bb_storage_txn_t. Single-use: commit()/discard() both
// close the wrapped txn.
typedef struct {
    bb_storage_txn_t txn;      // wrapped, unmodified -- never inspect its private fields
    const char *backend;       // session group key, set at begin()
    const char *ns_or_dir;     // session group key, set at begin()
    bb_err_t    _local_err;    // this layer's sticky error (first-wins)
    bool        _closed;       // set once commit()/discard() has run; single-use guard
} bb_config_staged_t;

// Begin a staged-write session against `backend`/`ns_or_dir`. Eagerly opens
// the wrapped txn via bb_storage_txn_begin() and records backend/ns_or_dir
// on h (compared against every subsequent set_*'s field addr). Delegated
// error contract -- same return codes as bb_storage_txn_begin().
bb_err_t bb_config_staged_begin(bb_config_staged_t *h, const char *backend, const char *ns_or_dir);

// Stage field f's value for later commit. If the session is already closed
// (a prior commit()/discard() has run), returns BB_ERR_INVALID_STATE without
// touching anything else. Otherwise, prechecks, in order: h/f non-NULL;
// f->type matches the setter's type (mirrors bb_config_set_*'s own type
// guard); f->addr.backend/ns_or_dir match the session's (cross-namespace
// staging is a caller bug); for str/blob, the value fits f->max_len (mirrors
// bb_config_set_str/_blob exactly). Any precheck failure sets h's local
// sticky error (first-wins) and returns WITHOUT touching the wrapped txn. If
// a local sticky error is already set, the call short-circuits immediately.
// On a passing precheck, the value is encoded exactly as bb_config_set_*
// would encode it and forwarded to bb_storage_txn_set() -- a delegated
// (txn-level) failure poisons the txn itself (not this layer's local sticky
// error) and is returned as-is.
// Returns:
//   BB_OK                 staged
//   BB_ERR_INVALID_ARG    h or f is NULL, f->type mismatch, f->addr targets
//                         a different backend/namespace than the session, or
//                         (str) strlen(v) >= f->max_len / (blob) len > f->max_len
//   BB_ERR_INVALID_STATE  the session is already closed (commit()/discard()
//                         already ran)
//   (other)                a delegated bb_storage_txn_set() error
bb_err_t bb_config_staged_set_bool(bb_config_staged_t *h, const bb_config_field_t *f, bool v);
bb_err_t bb_config_staged_set_u8(bb_config_staged_t *h, const bb_config_field_t *f, uint8_t v);
bb_err_t bb_config_staged_set_u16(bb_config_staged_t *h, const bb_config_field_t *f, uint16_t v);
bb_err_t bb_config_staged_set_u32(bb_config_staged_t *h, const bb_config_field_t *f, uint32_t v);
bb_err_t bb_config_staged_set_i32(bb_config_staged_t *h, const bb_config_field_t *f, int32_t v);
bb_err_t bb_config_staged_set_str(bb_config_staged_t *h, const bb_config_field_t *f, const char *v);
bb_err_t bb_config_staged_set_blob(bb_config_staged_t *h, const bb_config_field_t *f, const void *v, size_t len);

// Commit all staged writes atomically and durably, then close the session
// (single-use). If the session is already closed (a prior commit()/
// discard() has run), returns BB_ERR_INVALID_STATE immediately -- this is
// the ONLY thing a second call to commit() ever returns, whether the first
// call closed cleanly or via a local-precheck poison. Otherwise: if h's
// local sticky error is set (a precheck failure from some earlier set_*
// call), the wrapped txn is aborted (discarding whatever landed) and the
// local sticky error is returned WITHOUT calling bb_storage_txn_commit().
// Otherwise delegates straight to bb_storage_txn_commit() and returns its
// result unchanged (including its own sticky error, if a delegated set_*
// failure poisoned the txn). Either way, the session is marked closed
// before returning.
bb_err_t bb_config_staged_commit(bb_config_staged_t *h);

// Discard all staged writes and close the session. Aborts the wrapped txn
// via bb_storage_txn_abort() and marks the session closed. Idempotent:
// returns BB_OK on an already-closed (via commit() or a prior discard()) or
// never-begun (zero-initialized) handle; otherwise returns
// bb_storage_txn_abort()'s result (BB_OK on all shipped backends).
bb_err_t bb_config_staged_discard(bb_config_staged_t *h);

#ifdef __cplusplus
}
#endif
