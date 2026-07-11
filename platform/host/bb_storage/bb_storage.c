#include "bb_storage.h"
#include "bb_log.h"

#include <stddef.h>
#include <string.h>

static const char *TAG = "bb_storage";

/* ---------------------------------------------------------------------------
 * Internal backend entry
 * ---------------------------------------------------------------------------*/
typedef struct {
    const char           *name;
    bb_storage_vtable_t   vt;
    void                 *impl;
} bb_storage_backend_entry_t;

/* ---------------------------------------------------------------------------
 * File-scope state — no heap, no ESP, s_ prefix per house rules
 *
 * Registration-time-only contract: backends are registered once at
 * composition/init (single writer, per the "composition-only" convention in
 * bb_storage.h) — bb_storage_register_backend() is NOT concurrency-safe
 * against itself or against a concurrent get/set/erase/exists dispatch.
 * Per-call get/set/erase/exists dispatch (below) is read-only against this
 * registry once composition has finished, so no runtime lock is added here;
 * any locking a backend needs for its own data is that backend's concern
 * (see bb_storage_ram's s_lock for the RAM backend's contract).
 * ---------------------------------------------------------------------------*/
static bb_storage_backend_entry_t s_backends[BB_STORAGE_MAX_BACKENDS];
static size_t                     s_count;

void bb_storage_test_reset(void)
{
    memset(s_backends, 0, sizeof(s_backends));
    s_count = 0;
}

bb_err_t bb_storage_register_backend(const char *name, const bb_storage_vtable_t *vt, void *impl)
{
    if (name == NULL || vt == NULL || vt->get == NULL || vt->set == NULL ||
        vt->erase == NULL || vt->exists == NULL) {
        return BB_ERR_INVALID_ARG;
    }
    if ((vt->get_typed == NULL) != (vt->set_typed == NULL)) {
        // get_typed/set_typed are an optional pair — one set without the
        // other is a broken vtable, not a valid "typed unsupported" state.
        return BB_ERR_INVALID_ARG;
    }
    {
        // txn_begin/txn_set/txn_commit/txn_abort are an optional group of
        // four — all NULL or all set. A partial group is a broken vtable.
        int txn_set_count = (vt->txn_begin != NULL) + (vt->txn_set != NULL) +
                             (vt->txn_commit != NULL) + (vt->txn_abort != NULL);
        if (txn_set_count != 0 && txn_set_count != 4) {
            return BB_ERR_INVALID_ARG;
        }
    }

    for (size_t i = 0; i < s_count; i++) {
        if (strcmp(s_backends[i].name, name) == 0) {
            bb_log_w(TAG, "duplicate backend '%s' ignored (first registration wins)", name);
            return BB_ERR_INVALID_STATE;
        }
    }

    if (s_count >= BB_STORAGE_MAX_BACKENDS) {
        return BB_ERR_NO_SPACE;
    }

    s_backends[s_count].name = name;
    s_backends[s_count].vt   = *vt;
    s_backends[s_count].impl = impl;
    s_count++;

    return BB_OK;
}

static const bb_storage_backend_entry_t *find_backend(const char *name)
{
    if (name == NULL) {
        return NULL;
    }

    for (size_t i = 0; i < s_count; i++) {
        if (strcmp(s_backends[i].name, name) == 0) {
            return &s_backends[i];
        }
    }

    return NULL;
}

bb_err_t bb_storage_get(const bb_storage_addr_t *addr, void *buf, size_t cap, size_t *out_len)
{
    if (addr == NULL || addr->backend == NULL || out_len == NULL || (cap > 0 && buf == NULL)) {
        return BB_ERR_INVALID_ARG;
    }

    const bb_storage_backend_entry_t *entry = find_backend(addr->backend);
    if (entry == NULL) {
        return BB_ERR_NOT_FOUND;
    }

    return entry->vt.get(entry->impl, addr, buf, cap, out_len);
}

bb_err_t bb_storage_set(const bb_storage_addr_t *addr, const void *buf, size_t len)
{
    if (addr == NULL || addr->backend == NULL || (len > 0 && buf == NULL)) {
        return BB_ERR_INVALID_ARG;
    }

    const bb_storage_backend_entry_t *entry = find_backend(addr->backend);
    if (entry == NULL) {
        return BB_ERR_NOT_FOUND;
    }

    return entry->vt.set(entry->impl, addr, buf, len);
}

bb_err_t bb_storage_erase(const bb_storage_addr_t *addr)
{
    if (addr == NULL || addr->backend == NULL) {
        return BB_ERR_INVALID_ARG;
    }

    const bb_storage_backend_entry_t *entry = find_backend(addr->backend);
    if (entry == NULL) {
        return BB_ERR_NOT_FOUND;
    }

    return entry->vt.erase(entry->impl, addr);
}

bool bb_storage_exists(const bb_storage_addr_t *addr)
{
    if (addr == NULL || addr->backend == NULL) {
        return false;
    }

    const bb_storage_backend_entry_t *entry = find_backend(addr->backend);
    if (entry == NULL) {
        return false;
    }

    return entry->vt.exists(entry->impl, addr);
}

bb_err_t bb_storage_get_typed(const bb_storage_addr_t *addr, bb_storage_enc_t enc,
                               void *buf, size_t cap, size_t *out_len)
{
    if (addr == NULL || addr->backend == NULL || out_len == NULL || (cap > 0 && buf == NULL)) {
        return BB_ERR_INVALID_ARG;
    }

    const bb_storage_backend_entry_t *entry = find_backend(addr->backend);
    if (entry == NULL) {
        return BB_ERR_NOT_FOUND;
    }

    if (entry->vt.get_typed != NULL) {
        return entry->vt.get_typed(entry->impl, addr, enc, buf, cap, out_len);
    }
    return entry->vt.get(entry->impl, addr, buf, cap, out_len);
}

bb_err_t bb_storage_set_typed(const bb_storage_addr_t *addr, bb_storage_enc_t enc,
                               const void *buf, size_t len)
{
    if (addr == NULL || addr->backend == NULL || (len > 0 && buf == NULL)) {
        return BB_ERR_INVALID_ARG;
    }

    const bb_storage_backend_entry_t *entry = find_backend(addr->backend);
    if (entry == NULL) {
        return BB_ERR_NOT_FOUND;
    }

    if (entry->vt.set_typed != NULL) {
        return entry->vt.set_typed(entry->impl, addr, enc, buf, len);
    }
    return entry->vt.set(entry->impl, addr, buf, len);
}

/* ---------------------------------------------------------------------------
 * Multi-key transactions — thin dispatch to the backend hooks captured at
 * begin(). See bb_storage.h for the full contract.
 *
 * bb_storage_txn_t's _txn_set/_txn_commit/_txn_abort fields and
 * bb_storage_vtable_t's txn_set/txn_commit/txn_abort members now share the
 * same `bb_storage_txn_t *txn` signature (bb_storage.h forward-declares
 * bb_storage_txn_t ahead of its own definition for exactly this reason) —
 * assigned directly below, no function-pointer cast.
 * ---------------------------------------------------------------------------*/
bb_err_t bb_storage_txn_begin(const char *backend, const char *ns_or_dir, bb_storage_txn_t *txn)
{
    if (backend == NULL || ns_or_dir == NULL || txn == NULL) {
        return BB_ERR_INVALID_ARG;
    }

    if (txn->_open) {
        // Already open — caller must commit or abort before beginning again.
        return BB_ERR_INVALID_STATE;
    }

    const bb_storage_backend_entry_t *entry = find_backend(backend);
    if (entry == NULL) {
        return BB_ERR_NOT_FOUND;
    }

    if (entry->vt.txn_begin == NULL) {
        return BB_ERR_UNSUPPORTED;
    }

    memset(txn, 0, sizeof(*txn));
    // Capture the backend's fn-ptrs + impl by value here, once, rather than
    // re-resolving the backend by name on every txn_set/commit/abort call
    // (contrast get_typed/set_typed above, which do look up per-call — they
    // have no open/close lifecycle to amortize the lookup against). This
    // assumes the backend's registration (impl + vtable) outlives any txn
    // opened against it — true for the composition-only, register-once-at-
    // init backends this facade targets, but worth calling out since it's a
    // deliberate deviation from the rest of this file's per-call lookup
    // style, not an oversight.
    txn->_txn_set    = entry->vt.txn_set;
    txn->_txn_commit = entry->vt.txn_commit;
    txn->_txn_abort  = entry->vt.txn_abort;
    txn->_impl       = entry->impl;
    txn->_err        = BB_OK;

    return entry->vt.txn_begin(entry->impl, txn, ns_or_dir);
}

bb_err_t bb_storage_txn_set(bb_storage_txn_t *txn, const char *key, bb_storage_enc_t enc,
                             const void *buf, size_t len)
{
    if (txn == NULL || key == NULL) {
        return BB_ERR_INVALID_ARG;
    }
    if (!txn->_open) {
        return BB_ERR_INVALID_STATE;
    }
    if (txn->_err != BB_OK) {
        return txn->_err;
    }

    bb_err_t err = txn->_txn_set(txn->_impl, txn, key, enc, buf, len);
    if (err != BB_OK) {
        txn->_err = err;
    }
    return err;
}

bb_err_t bb_storage_txn_commit(bb_storage_txn_t *txn)
{
    if (txn == NULL) {
        return BB_ERR_INVALID_ARG;
    }
    if (!txn->_open) {
        // Symmetric with bb_storage_txn_set's INVALID_STATE guard above: a
        // commit on an already-committed/aborted or never-begun txn is a
        // caller bug, not a silent no-op success. bb_storage_txn_abort is
        // the ONE deliberate exception to this (see its own comment below)
        // — commit is not.
        return BB_ERR_INVALID_STATE;
    }

    bb_err_t err = txn->_txn_commit(txn->_impl, txn);
    if (err != BB_OK && txn->_err == BB_OK) {
        txn->_err = err;
    }
    return err;
}

bb_err_t bb_storage_txn_abort(bb_storage_txn_t *txn)
{
    // Deliberately idempotent, unlike bb_storage_txn_set/commit above: abort
    // is the caller's "give up, discard whatever happened" escape hatch, so
    // it must be safe to call on a poisoned, already-closed (committed or
    // previously aborted), or never-begun *zero-initialized* txn without the
    // caller having to track open/closed state itself — e.g. an
    // error-cleanup path that unconditionally calls abort() regardless of
    // how far a transaction got. Returning BB_ERR_INVALID_STATE here instead
    // would force every such cleanup path to track that state and guard the
    // call, for no safety benefit (there is nothing to discard).
    if (txn == NULL) {
        return BB_ERR_INVALID_ARG;
    }
    if (!txn->_open) {
        return BB_OK;
    }

    return txn->_txn_abort(txn->_impl, txn);
}
