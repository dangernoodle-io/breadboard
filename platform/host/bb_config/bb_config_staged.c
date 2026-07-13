// bb_config_staged — shared portable implementation (compiled host + espidf
// + arduino). No platform includes; only bb_config + bb_storage +
// bb_byte_order (bb_core).

#include "bb_config_staged.h"

#include <string.h>

#include "bb_byte_order.h"

// ---------------------------------------------------------------------------
// Local sticky-error helpers
// ---------------------------------------------------------------------------

// First-wins: never overwrite an existing local sticky error. Returns the
// (possibly just-set) sticky error for the caller to return directly.
static bb_err_t poison(bb_config_staged_t *h, bb_err_t err)
{
    if (h->_local_err == BB_OK) {
        h->_local_err = err;
    }
    return h->_local_err;
}

static bool str_eq(const char *a, const char *b)
{
    if (a == NULL || b == NULL) {
        return a == b;
    }
    return strcmp(a, b) == 0;
}

// Common precheck shared by every set_*: a closed session short-circuits
// first (BB_ERR_INVALID_STATE -- no more stages accepted); then an
// already-set local sticky error short-circuits before anything else is
// touched; then f->type must match `want`; then f->addr must target the
// session's backend/ns_or_dir. Returns BB_OK when the caller may proceed to
// encode + forward to the wrapped txn. h must already be known non-NULL by
// the caller.
static bb_err_t precheck(bb_config_staged_t *h, const bb_config_field_t *f, bb_config_type_t want)
{
    if (h->_closed) {
        // Single-use session: a closed handle accepts no more stages.
        return BB_ERR_INVALID_STATE;
    }
    if (h->_local_err != BB_OK) {
        return h->_local_err;
    }
    if (f == NULL || f->type != want) {
        return poison(h, BB_ERR_INVALID_ARG);
    }
    if (!str_eq(f->addr.backend, h->backend) || !str_eq(f->addr.ns_or_dir, h->ns_or_dir)) {
        return poison(h, BB_ERR_INVALID_ARG);
    }
    return BB_OK;
}

// Shared scalar stage path -- encode via the caller-supplied fixed-width
// buffer (already byte-encoded exactly as bb_config's own scalar_set would
// encode it) and forward to the wrapped txn. A delegated (txn-level)
// failure is returned as-is -- it poisons the txn itself, not this layer's
// local sticky error.
static bb_err_t stage_scalar(bb_config_staged_t *h, const bb_config_field_t *f, bb_config_type_t want,
                              const uint8_t *buf, size_t width)
{
    bb_err_t rc = precheck(h, f, want);
    if (rc != BB_OK) {
        return rc;
    }
    return bb_storage_txn_set(&h->txn, f->addr.key, bb_config_type_to_enc(f->type), buf, width);
}

// ---------------------------------------------------------------------------
// begin / commit / discard
// ---------------------------------------------------------------------------

bb_err_t bb_config_staged_begin(bb_config_staged_t *h, const char *backend, const char *ns_or_dir)
{
    if (h == NULL) {
        return BB_ERR_INVALID_ARG;
    }
    h->backend    = backend;
    h->ns_or_dir  = ns_or_dir;
    h->_local_err = BB_OK;
    h->_closed    = false;
    return bb_storage_txn_begin(backend, ns_or_dir, &h->txn);
}

bb_err_t bb_config_staged_commit(bb_config_staged_t *h)
{
    if (h == NULL) {
        return BB_ERR_INVALID_ARG;
    }
    if (h->_closed) {
        // Single-use session: a second commit() always reports the same
        // closed signal, regardless of whether the first call closed
        // cleanly or via a local-precheck poison.
        return BB_ERR_INVALID_STATE;
    }
    if (h->_local_err != BB_OK) {
        // A local precheck already poisoned this session -- discard whatever
        // landed in the (possibly still-open) wrapped txn and never call
        // bb_storage_txn_commit(). This is the load-bearing all-or-nothing
        // path: a local precheck failure must not let earlier successful
        // stages land.
        bb_storage_txn_abort(&h->txn);
        h->_closed = true;
        return h->_local_err;
    }
    bb_err_t rc = bb_storage_txn_commit(&h->txn);
    h->_closed  = true;
    return rc;
}

bb_err_t bb_config_staged_discard(bb_config_staged_t *h)
{
    if (h == NULL) {
        return BB_ERR_INVALID_ARG;
    }
    if (h->_closed) {
        return BB_OK;
    }
    bb_err_t rc = bb_storage_txn_abort(&h->txn);
    h->_closed  = true;
    return rc;
}

// ---------------------------------------------------------------------------
// bool / u8 / u16 / u32 / i32 -- fixed-width scalar encode, mirrors
// bb_config.c's scalar_set exactly.
// ---------------------------------------------------------------------------

bb_err_t bb_config_staged_set_bool(bb_config_staged_t *h, const bb_config_field_t *f, bool v)
{
    if (h == NULL) {
        return BB_ERR_INVALID_ARG;
    }
    uint8_t byte = v ? 1 : 0;
    return stage_scalar(h, f, BB_CONFIG_BOOL, &byte, bb_config_scalar_width(BB_CONFIG_BOOL));
}

bb_err_t bb_config_staged_set_u8(bb_config_staged_t *h, const bb_config_field_t *f, uint8_t v)
{
    if (h == NULL) {
        return BB_ERR_INVALID_ARG;
    }
    return stage_scalar(h, f, BB_CONFIG_U8, &v, bb_config_scalar_width(BB_CONFIG_U8));
}

bb_err_t bb_config_staged_set_u16(bb_config_staged_t *h, const bb_config_field_t *f, uint16_t v)
{
    if (h == NULL) {
        return BB_ERR_INVALID_ARG;
    }
    uint8_t buf[2];
    bb_store_le16(buf, v);
    return stage_scalar(h, f, BB_CONFIG_U16, buf, sizeof(buf));
}

bb_err_t bb_config_staged_set_u32(bb_config_staged_t *h, const bb_config_field_t *f, uint32_t v)
{
    if (h == NULL) {
        return BB_ERR_INVALID_ARG;
    }
    uint8_t buf[4];
    bb_store_le32(buf, v);
    return stage_scalar(h, f, BB_CONFIG_U32, buf, sizeof(buf));
}

bb_err_t bb_config_staged_set_i32(bb_config_staged_t *h, const bb_config_field_t *f, int32_t v)
{
    if (h == NULL) {
        return BB_ERR_INVALID_ARG;
    }
    uint32_t bits;
    memcpy(&bits, &v, sizeof(bits));
    uint8_t buf[4];
    bb_store_le32(buf, bits);
    return stage_scalar(h, f, BB_CONFIG_I32, buf, sizeof(buf));
}

// ---------------------------------------------------------------------------
// str / blob -- the raw bytes ARE the encoding (mirrors bb_config_set_str/
// _blob exactly, including the max_len precheck).
// ---------------------------------------------------------------------------

bb_err_t bb_config_staged_set_str(bb_config_staged_t *h, const bb_config_field_t *f, const char *v)
{
    if (h == NULL) {
        return BB_ERR_INVALID_ARG;
    }
    bb_err_t rc = precheck(h, f, BB_CONFIG_STR);
    if (rc != BB_OK) {
        return rc;
    }
    if (v == NULL) {
        return poison(h, BB_ERR_INVALID_ARG);
    }
    size_t len = strlen(v);
    if (len >= f->max_len) {
        return poison(h, BB_ERR_INVALID_ARG);
    }
    return bb_storage_txn_set(&h->txn, f->addr.key, BB_STORAGE_ENC_STR, v, len);
}

bb_err_t bb_config_staged_set_blob(bb_config_staged_t *h, const bb_config_field_t *f, const void *v, size_t len)
{
    if (h == NULL) {
        return BB_ERR_INVALID_ARG;
    }
    bb_err_t rc = precheck(h, f, BB_CONFIG_BLOB);
    if (rc != BB_OK) {
        return rc;
    }
    if (len > 0 && v == NULL) {
        return poison(h, BB_ERR_INVALID_ARG);
    }
    if (len > f->max_len) {
        return poison(h, BB_ERR_INVALID_ARG);
    }
    return bb_storage_txn_set(&h->txn, f->addr.key, BB_STORAGE_ENC_BLOB, v, len);
}
