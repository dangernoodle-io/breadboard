#include "bb_storage_nvs.h"
#include "bb_storage.h"
#include "bb_storage_nvs_classify_enc.h"
#include "bb_byte_order.h"
#include "bb_str.h"
#include "bb_log.h"
#include "bb_once.h"

#include <stdint.h>
#include <string.h>

static const char *TAG = "bb_storage_nvs";

#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#include "nvs_flash.h"
#include "nvs.h"
#endif

/* ---------------------------------------------------------------------------
 * NVS partition bring-up — bb_storage_nvs_flash_init()/_flash_was_erased()
 *
 * Moved verbatim (erase-and-retry logic byte-identical) from
 * platform/espidf/bb_nv/bb_nv.c's bb_nv_flash_init() (B1-840, bb_nv
 * dissolution epic B1-708) — bb_nv now forwards to this. See
 * bb_storage_nvs.h for the tier=early / idempotency rationale.
 * ---------------------------------------------------------------------------*/
#ifdef ESP_PLATFORM
static bb_err_t  s_flash_init_err;
static bool      s_flash_was_erased;
static bb_once_t s_flash_once = BB_ONCE_INIT;

static void flash_init_once(void *ctx)
{
    (void)ctx;
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        bb_log_e(TAG, "NVS erased on corruption — creds may be lost");
        s_flash_was_erased = true;
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    s_flash_init_err = err;
}

bb_err_t bb_storage_nvs_flash_init(void)
{
    bb_once_run(&s_flash_once, flash_init_once, NULL);
    return s_flash_init_err;
}

bool bb_storage_nvs_flash_was_erased(void)
{
    return s_flash_was_erased;
}
#else
#ifdef BB_STORAGE_NVS_TESTING
// Host testing seam: bb_storage_nvs_flash_init()'s real body only exists
// under ESP_PLATFORM (nvs_flash_init()/nvs_flash_erase() are device-only) --
// this seam drives the exact same bb_once_run(&once, fn, NULL) call that
// on-device flash_init_once() sits behind, against a fake counting body, to
// prove the once-guard runs the body exactly once across repeated calls.
static bb_once_t s_flash_once_test = BB_ONCE_INIT;
static int       s_flash_body_run_count_test;

static void flash_init_once_test(void *ctx)
{
    (void)ctx;
    s_flash_body_run_count_test++;
}

int bb_storage_nvs_flash_init_run_count_for_test(void)
{
    return s_flash_body_run_count_test;
}

void bb_storage_nvs_flash_init_reset_for_test(void)
{
    s_flash_once_test = (bb_once_t)BB_ONCE_INIT;
    s_flash_body_run_count_test = 0;
}

void bb_storage_nvs_flash_init_call_for_test(void)
{
    bb_once_run(&s_flash_once_test, flash_init_once_test, NULL);
}
#endif

bb_err_t bb_storage_nvs_flash_init(void)
{
    return BB_ERR_UNSUPPORTED;
}

bool bb_storage_nvs_flash_was_erased(void)
{
    return false;
}
#endif

/* ---------------------------------------------------------------------------
 * Multi-key transactions — NVS-primitive seam.
 *
 * nvs_txn_begin/set/commit/abort below (and the small ops table they drive,
 * bb_storage_nvs_txn_ops_t, declared in bb_storage_nvs.h) are portable —
 * compiled on host and device alike — so the open->set*->commit->close
 * orchestration can be host-tested against a fake without pulling in
 * ESP-IDF's nvs.h. On-device, s_txn_ops defaults to s_real_txn_ops (thin
 * wrappers around the real nvs_open, nvs_set_u8/u16/u32/i32/str/blob,
 * nvs_commit, and nvs_close calls,
 * including the existing type-mismatch-erase-and-retry policy for the
 * integer setters — see txn_set_u8_real et al. below). On host there is no
 * real backing store, so s_txn_ops starts NULL; a host test must call
 * bb_storage_nvs_set_txn_ops_for_test() to inject a fake before driving
 * nvs_txn_begin_for_test()/etc — an unset ops table fails closed
 * (BB_ERR_UNSUPPORTED), never a crash.
 *
 * Generalizes bb_nv_batch_* (platform/espidf/bb_nv/bb_nv.c) into the generic
 * bb_storage_vtable_t txn group: one NVS handle is opened at begin() and
 * held open across every txn_set, so every staged write goes through the
 * SAME NVS page-cache transaction; commit() at commit() makes the whole
 * batch atomic + durable in a single flash write, exactly like
 * bb_nv_batch_commit(). abort() closes the handle WITHOUT a commit —
 * uncommitted nvs_set_* writes are discarded when the handle is closed
 * without a commit, the same reliance bb_nv_batch_* already has via
 * bb_nv_batch_commit's identical close-without-commit-on-error path, not a
 * documented hard guarantee.
 * ---------------------------------------------------------------------------*/

// ESP-IDF's real NVS key-name limit is 15 chars + NUL (nvs.h's
// NVS_KEY_NAME_MAX_SIZE=16) — independent of BB_STORAGE_TXN_KEY_MAX_BYTES
// (bb_storage.h, 32), which is the generic facade/RAM ceiling, NOT the NVS
// limit. Named here (rather than a bare 16) so nvs_txn_set's key-length
// guard below is self-documenting; mirrors bb_nv.c's BB_NV_HOST_STR_KEY_MAX
// pattern. Kept portable (no nvs.h include) so this seam still compiles on
// host, where a fake ops table drives the same guard in tests.
#define BB_STORAGE_NVS_TXN_KEY_MAX_BYTES 16

#ifdef ESP_PLATFORM
static bb_err_t txn_open_real(const char *ns, uint32_t *out_handle)
{
    nvs_handle_t h;
    bb_err_t     err = nvs_open(ns, NVS_READWRITE, &h);
    if (err == BB_OK) {
        *out_handle = (uint32_t)h;
    }
    return err;
}

static bb_err_t txn_set_u8_real(uint32_t handle, const char *key, uint8_t value)
{
    nvs_handle_t h   = (nvs_handle_t)handle;
    bb_err_t     err = nvs_set_u8(h, key, value);
    if (err == ESP_ERR_NVS_TYPE_MISMATCH) {
        bb_log_w(TAG, "type mismatch on txn set '%s', rewriting", key);
        (void)nvs_erase_key(h, key);
        err = nvs_set_u8(h, key, value);
    }
    return err;
}

static bb_err_t txn_set_u16_real(uint32_t handle, const char *key, uint16_t value)
{
    nvs_handle_t h   = (nvs_handle_t)handle;
    bb_err_t     err = nvs_set_u16(h, key, value);
    if (err == ESP_ERR_NVS_TYPE_MISMATCH) {
        bb_log_w(TAG, "type mismatch on txn set '%s', rewriting", key);
        (void)nvs_erase_key(h, key);
        err = nvs_set_u16(h, key, value);
    }
    return err;
}

static bb_err_t txn_set_u32_real(uint32_t handle, const char *key, uint32_t value)
{
    nvs_handle_t h   = (nvs_handle_t)handle;
    bb_err_t     err = nvs_set_u32(h, key, value);
    if (err == ESP_ERR_NVS_TYPE_MISMATCH) {
        bb_log_w(TAG, "type mismatch on txn set '%s', rewriting", key);
        (void)nvs_erase_key(h, key);
        err = nvs_set_u32(h, key, value);
    }
    return err;
}

static bb_err_t txn_set_i32_real(uint32_t handle, const char *key, int32_t value)
{
    nvs_handle_t h   = (nvs_handle_t)handle;
    bb_err_t     err = nvs_set_i32(h, key, value);
    if (err == ESP_ERR_NVS_TYPE_MISMATCH) {
        bb_log_w(TAG, "type mismatch on txn set '%s', rewriting", key);
        (void)nvs_erase_key(h, key);
        err = nvs_set_i32(h, key, value);
    }
    return err;
}

static bb_err_t txn_set_str_real(uint32_t handle, const char *key, const char *value)
{
    return nvs_set_str((nvs_handle_t)handle, key, value);
}

static bb_err_t txn_set_blob_real(uint32_t handle, const char *key, const void *buf, size_t len)
{
    return nvs_set_blob((nvs_handle_t)handle, key, buf, len);
}

static bb_err_t txn_commit_real(uint32_t handle)
{
    return nvs_commit((nvs_handle_t)handle);
}

static void txn_close_real(uint32_t handle)
{
    nvs_close((nvs_handle_t)handle);
}

static const bb_storage_nvs_txn_ops_t s_real_txn_ops = {
    .open     = txn_open_real,
    .set_u8   = txn_set_u8_real,
    .set_u16  = txn_set_u16_real,
    .set_u32  = txn_set_u32_real,
    .set_i32  = txn_set_i32_real,
    .set_str  = txn_set_str_real,
    .set_blob = txn_set_blob_real,
    .commit   = txn_commit_real,
    .close    = txn_close_real,
};

static const bb_storage_nvs_txn_ops_t *s_txn_ops = &s_real_txn_ops;
#else
// Host: no real NVS to back the txn ops — a test must inject a fake via
// bb_storage_nvs_set_txn_ops_for_test() before driving the txn functions
// below; an unset ops table fails every call closed (BB_ERR_UNSUPPORTED).
static const bb_storage_nvs_txn_ops_t *s_txn_ops = NULL;
#endif

#ifdef BB_STORAGE_NVS_TESTING
void bb_storage_nvs_set_txn_ops_for_test(const bb_storage_nvs_txn_ops_t *ops)
{
#ifdef ESP_PLATFORM
    s_txn_ops = (ops != NULL) ? ops : &s_real_txn_ops;
#else
    s_txn_ops = ops;
#endif
}
#endif

static bb_err_t nvs_txn_begin(void *impl, bb_storage_txn_t *txn, const char *ns_or_dir)
{
    (void)impl;

    if (s_txn_ops == NULL) {
        txn->_err = BB_ERR_UNSUPPORTED;
        return BB_ERR_UNSUPPORTED;
    }

    uint32_t h   = 0;
    bb_err_t err = s_txn_ops->open(ns_or_dir, &h);
    if (err != BB_OK) {
        txn->_err = err;
        return err;
    }

    txn->_handle = (uintptr_t)h;
    txn->_open   = 1;
    txn->_err    = BB_OK;
    return BB_OK;
}

static bb_err_t nvs_txn_set(void *impl, bb_storage_txn_t *txn, const char *key, bb_storage_enc_t enc,
                             const void *buf, size_t len)
{
    (void)impl;

    if (!txn->_open) {
        return BB_ERR_INVALID_STATE;
    }
    if (txn->_err != BB_OK) {
        return txn->_err;
    }

    uint32_t h = (uint32_t)txn->_handle;
    bb_err_t err;

    // Fail fast on a key that exceeds NVS's real key-name limit (15 chars +
    // NUL, ESP-IDF's NVS_KEY_NAME_MAX_SIZE=16) rather than deferring to a
    // confusing deep-ESP-IDF rejection. BB_STORAGE_TXN_KEY_MAX_BYTES (32) is
    // a generic-facade/RAM ceiling, not the NVS limit — see bb_storage.h.
    if (key == NULL || strlen(key) >= BB_STORAGE_NVS_TXN_KEY_MAX_BYTES) {
        err = BB_ERR_INVALID_ARG;
        txn->_err = err;
        return err;
    }

    // Uniform cap ahead of the encoding switch: every encoding (STR, BLOB,
    // and the fixed-size scalars below) is bounded by the same value-size
    // ceiling, so an oversize write is rejected BEFORE any write is
    // dispatched to the backend — matches the RAM backend's contract
    // (platform/host/bb_storage_ram/bb_storage_ram.c) and the header's
    // documented BB_ERR_NO_SPACE-on-oversize-value contract.
    if (len > BB_STORAGE_TXN_VALUE_MAX_BYTES) {
        err = BB_ERR_NO_SPACE;
        txn->_err = err;
        return err;
    }

    switch (bb_storage_nvs_classify_enc(enc)) {
    case BB_STORAGE_NVS_KIND_STR: {
        char scratch[BB_STORAGE_TXN_VALUE_MAX_BYTES + 1];
        if (len > 0) {
            memcpy(scratch, buf, len);
        }
        scratch[len] = '\0';
        err = s_txn_ops->set_str(h, key, scratch);
        break;
    }

    case BB_STORAGE_NVS_KIND_U8:
        if (len != sizeof(uint8_t)) { err = BB_ERR_INVALID_ARG; break; }
        err = s_txn_ops->set_u8(h, key, ((const uint8_t *)buf)[0]);
        break;

    case BB_STORAGE_NVS_KIND_U16:
        if (len != sizeof(uint16_t)) { err = BB_ERR_INVALID_ARG; break; }
        err = s_txn_ops->set_u16(h, key, bb_load_le16(buf));
        break;

    case BB_STORAGE_NVS_KIND_U32:
        if (len != sizeof(uint32_t)) { err = BB_ERR_INVALID_ARG; break; }
        err = s_txn_ops->set_u32(h, key, bb_load_le32(buf));
        break;

    case BB_STORAGE_NVS_KIND_I32: {
        if (len != sizeof(uint32_t)) { err = BB_ERR_INVALID_ARG; break; }
        uint32_t bits = bb_load_le32(buf);
        int32_t  value;
        memcpy(&value, &bits, sizeof(value));
        err = s_txn_ops->set_i32(h, key, value);
        break;
    }

    case BB_STORAGE_NVS_KIND_BLOB:
    default:
        err = s_txn_ops->set_blob(h, key, buf, len);
        break;
    }

    if (err != BB_OK) {
        txn->_err = err;
    }
    return err;
}

static bb_err_t nvs_txn_commit(void *impl, bb_storage_txn_t *txn)
{
    (void)impl;

    if (!txn->_open) {
        return BB_ERR_INVALID_STATE;
    }

    uint32_t h   = (uint32_t)txn->_handle;
    bb_err_t err = txn->_err;
    if (err == BB_OK) {
        err = s_txn_ops->commit(h);
    }
    s_txn_ops->close(h);
    txn->_open = 0;

    if (err != BB_OK && txn->_err == BB_OK) {
        txn->_err = err;
    }
    return err;
}

static bb_err_t nvs_txn_abort(void *impl, bb_storage_txn_t *txn)
{
    (void)impl;

    if (!txn->_open) {
        return BB_OK;
    }

    uint32_t h = (uint32_t)txn->_handle;
    s_txn_ops->close(h);
    txn->_open = 0;
    return BB_OK;
}

#ifdef BB_STORAGE_NVS_TESTING
bb_err_t bb_storage_nvs_txn_begin_for_test(bb_storage_txn_t *txn, const char *ns_or_dir)
{
    return nvs_txn_begin(NULL, txn, ns_or_dir);
}

bb_err_t bb_storage_nvs_txn_set_for_test(bb_storage_txn_t *txn, const char *key, bb_storage_enc_t enc,
                                          const void *buf, size_t len)
{
    return nvs_txn_set(NULL, txn, key, enc, buf, len);
}

bb_err_t bb_storage_nvs_txn_commit_for_test(bb_storage_txn_t *txn)
{
    return nvs_txn_commit(NULL, txn);
}

bb_err_t bb_storage_nvs_txn_abort_for_test(bb_storage_txn_t *txn)
{
    return nvs_txn_abort(NULL, txn);
}
#endif

#ifdef ESP_PLATFORM
#include "bb_storage_nvs_get_decision.h"

// ---------------------------------------------------------------------------
// Capacity constants (Kconfig bridge — pattern from bb_clock.h)
// ---------------------------------------------------------------------------
#ifdef CONFIG_BB_STORAGE_NVS_GET_SCRATCH_MAX
#define BB_STORAGE_NVS_GET_SCRATCH_MAX CONFIG_BB_STORAGE_NVS_GET_SCRATCH_MAX
#endif
#ifndef BB_STORAGE_NVS_GET_SCRATCH_MAX
#define BB_STORAGE_NVS_GET_SCRATCH_MAX 512
#endif

/* ---------------------------------------------------------------------------
 * Typed single-key accessors — moved verbatim from platform/espidf/bb_nv/
 * bb_nv.c (same nvs_open/get/set/erase-per-call shape, same type-mismatch
 * handling), renamed. bb_nv_set_u8/get_u8/etc forward to these unchanged.
 * ---------------------------------------------------------------------------*/
bb_err_t bb_storage_nvs_set_u8(const char *ns, const char *key, uint8_t value)
{
    if (ns == NULL || key == NULL) {
        return BB_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    bb_err_t err = nvs_open(ns, NVS_READWRITE, &handle);
    if (err != BB_OK) return err;

    err = nvs_set_u8(handle, key, value);
    if (err == ESP_ERR_NVS_TYPE_MISMATCH) {
        bb_log_w(TAG, "type mismatch on set '%s/%s', rewriting", ns, key);
        (void)nvs_erase_key(handle, key);
        err = nvs_set_u8(handle, key, value);
    }
    if (err == BB_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    return err;
}

bb_err_t bb_storage_nvs_set_u16(const char *ns, const char *key, uint16_t value)
{
    if (ns == NULL || key == NULL) {
        return BB_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    bb_err_t err = nvs_open(ns, NVS_READWRITE, &handle);
    if (err != BB_OK) return err;

    err = nvs_set_u16(handle, key, value);
    if (err == ESP_ERR_NVS_TYPE_MISMATCH) {
        bb_log_w(TAG, "type mismatch on set '%s/%s', rewriting", ns, key);
        (void)nvs_erase_key(handle, key);
        err = nvs_set_u16(handle, key, value);
    }
    if (err == BB_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    return err;
}

bb_err_t bb_storage_nvs_set_u32(const char *ns, const char *key, uint32_t value)
{
    if (ns == NULL || key == NULL) {
        return BB_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    bb_err_t err = nvs_open(ns, NVS_READWRITE, &handle);
    if (err != BB_OK) return err;

    err = nvs_set_u32(handle, key, value);
    if (err == ESP_ERR_NVS_TYPE_MISMATCH) {
        bb_log_w(TAG, "type mismatch on set '%s/%s', rewriting", ns, key);
        (void)nvs_erase_key(handle, key);
        err = nvs_set_u32(handle, key, value);
    }
    if (err == BB_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    return err;
}

bb_err_t bb_storage_nvs_set_i32(const char *ns, const char *key, int32_t value)
{
    if (ns == NULL || key == NULL) {
        return BB_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    bb_err_t err = nvs_open(ns, NVS_READWRITE, &handle);
    if (err != BB_OK) return err;

    err = nvs_set_i32(handle, key, value);
    if (err == ESP_ERR_NVS_TYPE_MISMATCH) {
        bb_log_w(TAG, "type mismatch on set '%s/%s', rewriting", ns, key);
        (void)nvs_erase_key(handle, key);
        err = nvs_set_i32(handle, key, value);
    }
    if (err == BB_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    return err;
}

bb_err_t bb_storage_nvs_set_str(const char *ns, const char *key, const char *value)
{
    if (ns == NULL || key == NULL || value == NULL) {
        return BB_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    bb_err_t err = nvs_open(ns, NVS_READWRITE, &handle);
    if (err != BB_OK) return err;

    err = nvs_set_str(handle, key, value);
    if (err == BB_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    return err;
}

bb_err_t bb_storage_nvs_get_u8(const char *ns, const char *key, uint8_t *out, uint8_t fallback)
{
    if (ns == NULL || key == NULL || out == NULL) {
        return BB_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    bb_err_t err = nvs_open(ns, NVS_READONLY, &handle);

    if (err == ESP_ERR_NVS_NOT_FOUND || err == BB_ERR_NOT_INITIALIZED) {
        *out = fallback;
        return BB_OK;
    }

    if (err != BB_OK) {
        return err;
    }

    err = nvs_get_u8(handle, key, out);
    nvs_close(handle);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        *out = fallback;
        return BB_OK;
    }
    if (err == ESP_ERR_NVS_TYPE_MISMATCH) {
        bb_log_w(TAG, "type mismatch on get '%s/%s', using fallback", ns, key);
        *out = fallback;
        return BB_OK;
    }

    return err;
}

bb_err_t bb_storage_nvs_get_u16(const char *ns, const char *key, uint16_t *out, uint16_t fallback)
{
    if (ns == NULL || key == NULL || out == NULL) {
        return BB_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    bb_err_t err = nvs_open(ns, NVS_READONLY, &handle);

    if (err == ESP_ERR_NVS_NOT_FOUND || err == BB_ERR_NOT_INITIALIZED) {
        *out = fallback;
        return BB_OK;
    }

    if (err != BB_OK) {
        return err;
    }

    err = nvs_get_u16(handle, key, out);
    nvs_close(handle);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        *out = fallback;
        return BB_OK;
    }
    if (err == ESP_ERR_NVS_TYPE_MISMATCH) {
        bb_log_w(TAG, "type mismatch on get '%s/%s', using fallback", ns, key);
        *out = fallback;
        return BB_OK;
    }

    return err;
}

bb_err_t bb_storage_nvs_get_u32(const char *ns, const char *key, uint32_t *out, uint32_t fallback)
{
    if (ns == NULL || key == NULL || out == NULL) {
        return BB_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    bb_err_t err = nvs_open(ns, NVS_READONLY, &handle);

    if (err == ESP_ERR_NVS_NOT_FOUND || err == BB_ERR_NOT_INITIALIZED) {
        *out = fallback;
        return BB_OK;
    }

    if (err != BB_OK) {
        return err;
    }

    err = nvs_get_u32(handle, key, out);
    nvs_close(handle);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        *out = fallback;
        return BB_OK;
    }
    if (err == ESP_ERR_NVS_TYPE_MISMATCH) {
        bb_log_w(TAG, "type mismatch on get '%s/%s', using fallback", ns, key);
        *out = fallback;
        return BB_OK;
    }

    return err;
}

bb_err_t bb_storage_nvs_get_i32(const char *ns, const char *key, int32_t *out, int32_t fallback)
{
    if (ns == NULL || key == NULL || out == NULL) {
        return BB_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    bb_err_t err = nvs_open(ns, NVS_READONLY, &handle);

    if (err == ESP_ERR_NVS_NOT_FOUND || err == BB_ERR_NOT_INITIALIZED) {
        *out = fallback;
        return BB_OK;
    }

    if (err != BB_OK) {
        return err;
    }

    err = nvs_get_i32(handle, key, out);
    nvs_close(handle);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        *out = fallback;
        return BB_OK;
    }
    if (err == ESP_ERR_NVS_TYPE_MISMATCH) {
        bb_log_w(TAG, "type mismatch on get '%s/%s', using fallback", ns, key);
        *out = fallback;
        return BB_OK;
    }

    return err;
}

bb_err_t bb_storage_nvs_get_str(const char *ns, const char *key, char *buf, size_t len, const char *fallback)
{
    if (ns == NULL || key == NULL || buf == NULL || len == 0) {
        return BB_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    bb_err_t err = nvs_open(ns, NVS_READONLY, &handle);

    if (err == ESP_ERR_NVS_NOT_FOUND || err == BB_ERR_NOT_INITIALIZED) {
        if (fallback == NULL) {
            buf[0] = '\0';
        } else {
            bb_strlcpy(buf, fallback, len);
        }
        return BB_OK;
    }

    if (err != BB_OK) {
        return err;
    }

    size_t buf_len = len;
    err = nvs_get_str(handle, key, buf, &buf_len);
    nvs_close(handle);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        if (fallback == NULL) {
            buf[0] = '\0';
        } else {
            bb_strlcpy(buf, fallback, len);
        }
        return BB_OK;
    }

    return err;
}

bb_err_t bb_storage_nvs_erase(const char *ns, const char *key)
{
    if (ns == NULL || key == NULL) {
        return BB_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    bb_err_t err = nvs_open(ns, NVS_READWRITE, &handle);
    if (err != BB_OK) return err;

    err = nvs_erase_key(handle, key);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = BB_OK;
    }
    if (err == BB_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    return err;
}

bb_err_t bb_storage_nvs_erase_namespace(const char *ns)
{
    if (ns == NULL) {
        return BB_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    bb_err_t err = nvs_open(ns, NVS_READWRITE, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        /* Namespace doesn't exist — already clean, treat as success. */
        return BB_OK;
    }
    if (err != BB_OK) return err;

    err = nvs_erase_all(handle);
    if (err == BB_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    return err;
}

bool bb_storage_nvs_exists(const char *ns, const char *key)
{
    if (ns == NULL || key == NULL) return false;

    nvs_handle_t handle;
    bb_err_t err = nvs_open(ns, NVS_READONLY, &handle);
    if (err != BB_OK) return false;

    size_t required = 0;
    esp_err_t gerr = nvs_get_str(handle, key, NULL, &required);
    nvs_close(handle);

    /* required includes the NUL terminator; > 1 means at least one real byte */
    return (gerr == ESP_OK && required > 1);
}

/* ---------------------------------------------------------------------------
 * Generic blob-shaped bb_storage_vtable_t — the "nvs" backend as seen through
 * the portable bb_storage_get/set/erase/exists facade. addr->ns_or_dir is the
 * NVS namespace, addr->key is the NVS key. This path is independent of the
 * typed accessors above and is never used by bb_nv's forwarders.
 * ---------------------------------------------------------------------------*/
static bb_err_t nvs_vt_get(void *impl, const bb_storage_addr_t *addr, void *buf, size_t cap, size_t *out_len)
{
    (void)impl;
    if (addr->ns_or_dir == NULL || addr->key == NULL) {
        return BB_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    bb_err_t err = nvs_open(addr->ns_or_dir, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return BB_ERR_NOT_FOUND;
    if (err != BB_OK) return err;

    size_t required = 0;
    esp_err_t gerr = nvs_get_blob(handle, addr->key, NULL, &required);
    if (gerr == ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(handle);
        return BB_ERR_NOT_FOUND;
    }
    if (gerr != ESP_OK) {
        nvs_close(handle);
        return gerr;
    }

    bb_storage_nvs_get_outcome_t outcome =
        bb_storage_nvs_get_decide(required, cap, BB_STORAGE_NVS_GET_SCRATCH_MAX, 0, out_len);

    switch (outcome) {
    case BB_STORAGE_NVS_GET_PROBE:
        nvs_close(handle);
        return BB_OK;

    case BB_STORAGE_NVS_GET_FULL: {
        size_t read_len = required;
        gerr = nvs_get_blob(handle, addr->key, buf, &read_len);
        nvs_close(handle);
        return (gerr == ESP_OK) ? BB_OK : gerr;
    }

    case BB_STORAGE_NVS_GET_BOUNCE: {
        /* Truncating read: bounce through a bounded on-stack scratch buffer
         * (no heap). */
        uint8_t scratch[BB_STORAGE_NVS_GET_SCRATCH_MAX];
        size_t read_len = required;
        gerr = nvs_get_blob(handle, addr->key, scratch, &read_len);
        nvs_close(handle);
        if (gerr != ESP_OK) {
            return gerr;
        }
        memcpy(buf, scratch, cap);
        return BB_OK;
    }

    case BB_STORAGE_NVS_GET_NO_SPACE:
    default:
        /* required > scratch_max: a blob this large that also needs
         * truncation cannot be safely staged — refuse rather than silently
         * truncating past what we can bounce through. */
        nvs_close(handle);
        return BB_ERR_NO_SPACE;
    }
}

static bb_err_t nvs_vt_set(void *impl, const bb_storage_addr_t *addr, const void *buf, size_t len)
{
    (void)impl;
    if (addr->ns_or_dir == NULL || addr->key == NULL) {
        return BB_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    bb_err_t err = nvs_open(addr->ns_or_dir, NVS_READWRITE, &handle);
    if (err != BB_OK) return err;

    err = nvs_set_blob(handle, addr->key, buf, len);
    if (err == BB_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    return err;
}

static bb_err_t nvs_vt_erase(void *impl, const bb_storage_addr_t *addr)
{
    (void)impl;
    if (addr->ns_or_dir == NULL || addr->key == NULL) {
        return BB_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    bb_err_t err = nvs_open(addr->ns_or_dir, NVS_READWRITE, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return BB_OK;
    if (err != BB_OK) return err;

    err = nvs_erase_key(handle, addr->key);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = BB_OK;
    }
    if (err == BB_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    return err;
}

static bool nvs_vt_exists(void *impl, const bb_storage_addr_t *addr)
{
    (void)impl;
    if (addr->ns_or_dir == NULL || addr->key == NULL) return false;

    nvs_handle_t handle;
    bb_err_t err = nvs_open(addr->ns_or_dir, NVS_READONLY, &handle);
    if (err != BB_OK) return false;

    size_t required = 0;
    esp_err_t gerr = nvs_get_blob(handle, addr->key, NULL, &required);
    nvs_close(handle);

    return gerr == ESP_OK;
}

/* ---------------------------------------------------------------------------
 * Typed vtable hooks — native NVS typed entries (nvs_get/set_u8/u16/u32/i32/
 * str) instead of the generic blob path above, so the on-flash NVS type tag
 * matches what a provisioned board already wrote via bb_nv_set_str/etc.
 * These are OPTIONAL bb_storage_vtable_t members (see bb_storage.h) — BLOB
 * enc (the default) falls through to the existing blob get/set unchanged.
 * ---------------------------------------------------------------------------*/

// No-fallback scalar get variants: unlike bb_storage_nvs_get_uN (which bakes
// in a caller-supplied fallback and always returns BB_OK), the typed facade
// path needs BB_ERR_NOT_FOUND to propagate so bb_config's has_default logic
// can decide what happens next. A type mismatch is treated the same as
// not-found (logged) rather than surfacing a raw NVS error to the facade.
static bb_err_t nvs_get_u8_no_fallback(const char *ns, const char *key, uint8_t *out)
{
    nvs_handle_t handle;
    bb_err_t err = nvs_open(ns, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND || err == BB_ERR_NOT_INITIALIZED) return BB_ERR_NOT_FOUND;
    if (err != BB_OK) return err;

    err = nvs_get_u8(handle, key, out);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return BB_ERR_NOT_FOUND;
    if (err == ESP_ERR_NVS_TYPE_MISMATCH) {
        bb_log_w(TAG, "type mismatch on typed get '%s/%s', treating as not-found", ns, key);
        return BB_ERR_NOT_FOUND;
    }
    return err;
}

static bb_err_t nvs_get_u16_no_fallback(const char *ns, const char *key, uint16_t *out)
{
    nvs_handle_t handle;
    bb_err_t err = nvs_open(ns, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND || err == BB_ERR_NOT_INITIALIZED) return BB_ERR_NOT_FOUND;
    if (err != BB_OK) return err;

    err = nvs_get_u16(handle, key, out);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return BB_ERR_NOT_FOUND;
    if (err == ESP_ERR_NVS_TYPE_MISMATCH) {
        bb_log_w(TAG, "type mismatch on typed get '%s/%s', treating as not-found", ns, key);
        return BB_ERR_NOT_FOUND;
    }
    return err;
}

static bb_err_t nvs_get_u32_no_fallback(const char *ns, const char *key, uint32_t *out)
{
    nvs_handle_t handle;
    bb_err_t err = nvs_open(ns, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND || err == BB_ERR_NOT_INITIALIZED) return BB_ERR_NOT_FOUND;
    if (err != BB_OK) return err;

    err = nvs_get_u32(handle, key, out);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return BB_ERR_NOT_FOUND;
    if (err == ESP_ERR_NVS_TYPE_MISMATCH) {
        bb_log_w(TAG, "type mismatch on typed get '%s/%s', treating as not-found", ns, key);
        return BB_ERR_NOT_FOUND;
    }
    return err;
}

static bb_err_t nvs_get_i32_no_fallback(const char *ns, const char *key, int32_t *out)
{
    nvs_handle_t handle;
    bb_err_t err = nvs_open(ns, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND || err == BB_ERR_NOT_INITIALIZED) return BB_ERR_NOT_FOUND;
    if (err != BB_OK) return err;

    err = nvs_get_i32(handle, key, out);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return BB_ERR_NOT_FOUND;
    if (err == ESP_ERR_NVS_TYPE_MISMATCH) {
        bb_log_w(TAG, "type mismatch on typed get '%s/%s', treating as not-found", ns, key);
        return BB_ERR_NOT_FOUND;
    }
    return err;
}

// STR get: nvs_get_str is all-or-nothing (no partial-read API), so probe the
// true length first and reuse bb_storage_nvs_get_decide() for the
// PROBE/FULL/BOUNCE/NO_SPACE branch selection — same shape as nvs_vt_get's
// blob path. decide()'s `required` here is the string length WITHOUT the
// NUL, matching bb_storage's set(len=strlen)/out_len contract; nvs_get_str's
// own probe value (`probed`) counts the NUL, so it is required+1 bytes on
// the wire. nvs_get_str additionally requires the caller-supplied length to
// cover the NUL when reading straight into a buffer — decide() is called
// with reserve=1 so the FULL/BOUNCE boundary accounts for that extra byte:
// cap == str_len (no room for the NUL) correctly takes BOUNCE, not FULL.
static bb_err_t nvs_vt_get_typed_str(const bb_storage_addr_t *addr, void *buf, size_t cap, size_t *out_len)
{
    nvs_handle_t handle;
    bb_err_t err = nvs_open(addr->ns_or_dir, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return BB_ERR_NOT_FOUND;
    if (err != BB_OK) return err;

    size_t probed = 0;
    esp_err_t gerr = nvs_get_str(handle, addr->key, NULL, &probed);
    if (gerr == ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(handle);
        return BB_ERR_NOT_FOUND;
    }
    if (gerr == ESP_ERR_NVS_TYPE_MISMATCH) {
        nvs_close(handle);
        bb_log_w(TAG, "type mismatch on typed get '%s/%s', treating as not-found",
                 addr->ns_or_dir, addr->key);
        return BB_ERR_NOT_FOUND;
    }
    if (gerr != ESP_OK) {
        nvs_close(handle);
        return gerr;
    }

    size_t str_len = (probed > 0) ? probed - 1 : 0;

    bb_storage_nvs_get_outcome_t outcome =
        bb_storage_nvs_get_decide(str_len, cap, BB_STORAGE_NVS_GET_SCRATCH_MAX, 1, out_len);

    switch (outcome) {
    case BB_STORAGE_NVS_GET_PROBE:
        nvs_close(handle);
        return BB_OK;

    case BB_STORAGE_NVS_GET_FULL: {
        /* cap >= str_len + 1 (reserve=1): the caller's buf has room for the
         * string bytes plus the NUL nvs_get_str insists on writing, so it is
         * safe to read directly into it, same as the blob path's FULL
         * branch. read_len must be initialized to the caller's capacity
         * (nvs_get_str's in/out contract), never to `probed`. */
        size_t read_len = cap;
        gerr = nvs_get_str(handle, addr->key, (char *)buf, &read_len);
        nvs_close(handle);
        if (gerr == ESP_ERR_NVS_INVALID_LENGTH) {
            return BB_ERR_NO_SPACE;
        }
        return (gerr == ESP_OK) ? BB_OK : gerr;
    }

    case BB_STORAGE_NVS_GET_BOUNCE: {
        /* 0 < cap < str_len+1, and decide() guarantees str_len+1 <=
         * scratch_max here, so the bounded on-stack scratch is always large
         * enough for the full string (plus NUL) — never the caller's buf. */
        char scratch[BB_STORAGE_NVS_GET_SCRATCH_MAX];
        size_t read_len = probed;
        gerr = nvs_get_str(handle, addr->key, scratch, &read_len);
        nvs_close(handle);
        if (gerr == ESP_ERR_NVS_INVALID_LENGTH) {
            return BB_ERR_NO_SPACE;
        }
        if (gerr != ESP_OK) {
            return gerr;
        }
        /* scratch is NUL-terminated by nvs_get_str; bb_strlcpy guarantees
         * buf is too (truncating safely) even though this is a truncating
         * read -- restores the NUL guarantee the old bb_nv_get_str path had
         * via bb_strlcpy before the B1-756 typed-vtable migration dropped it
         * (B1-947). out_len (set by bb_storage_nvs_get_decide above) still
         * reports the FULL stored length, unaffected by this truncation. */
        bb_strlcpy((char *)buf, scratch, cap);
        return BB_OK;
    }

    case BB_STORAGE_NVS_GET_NO_SPACE:
    default:
        nvs_close(handle);
        return BB_ERR_NO_SPACE;
    }
}

static bb_err_t nvs_vt_get_typed(void *impl, const bb_storage_addr_t *addr, bb_storage_enc_t enc,
                                  void *buf, size_t cap, size_t *out_len)
{
    if (addr->ns_or_dir == NULL || addr->key == NULL || out_len == NULL) {
        return BB_ERR_INVALID_ARG;
    }

    switch (bb_storage_nvs_classify_enc(enc)) {
    case BB_STORAGE_NVS_KIND_STR:
        return nvs_vt_get_typed_str(addr, buf, cap, out_len);

    case BB_STORAGE_NVS_KIND_U8: {
        uint8_t v = 0;
        bb_err_t rc = nvs_get_u8_no_fallback(addr->ns_or_dir, addr->key, &v);
        if (rc != BB_OK) return rc;
        *out_len = sizeof(v);
        if (cap == 0) return BB_OK;
        if (cap < sizeof(v)) return BB_ERR_NO_SPACE;
        ((uint8_t *)buf)[0] = v;
        return BB_OK;
    }

    case BB_STORAGE_NVS_KIND_U16: {
        uint16_t v = 0;
        bb_err_t rc = nvs_get_u16_no_fallback(addr->ns_or_dir, addr->key, &v);
        if (rc != BB_OK) return rc;
        *out_len = sizeof(v);
        if (cap == 0) return BB_OK;
        if (cap < sizeof(v)) return BB_ERR_NO_SPACE;
        bb_store_le16(buf, v);
        return BB_OK;
    }

    case BB_STORAGE_NVS_KIND_U32: {
        uint32_t v = 0;
        bb_err_t rc = nvs_get_u32_no_fallback(addr->ns_or_dir, addr->key, &v);
        if (rc != BB_OK) return rc;
        *out_len = sizeof(v);
        if (cap == 0) return BB_OK;
        if (cap < sizeof(v)) return BB_ERR_NO_SPACE;
        bb_store_le32(buf, v);
        return BB_OK;
    }

    case BB_STORAGE_NVS_KIND_I32: {
        int32_t v = 0;
        bb_err_t rc = nvs_get_i32_no_fallback(addr->ns_or_dir, addr->key, &v);
        if (rc != BB_OK) return rc;
        uint32_t bits;
        memcpy(&bits, &v, sizeof(bits));
        *out_len = sizeof(bits);
        if (cap == 0) return BB_OK;
        if (cap < sizeof(bits)) return BB_ERR_NO_SPACE;
        bb_store_le32(buf, bits);
        return BB_OK;
    }

    case BB_STORAGE_NVS_KIND_BLOB:
    default:
        return nvs_vt_get(impl, addr, buf, cap, out_len);
    }
}

static bb_err_t nvs_vt_set_typed(void *impl, const bb_storage_addr_t *addr, bb_storage_enc_t enc,
                                  const void *buf, size_t len)
{
    if (addr->ns_or_dir == NULL || addr->key == NULL) {
        return BB_ERR_INVALID_ARG;
    }

    switch (bb_storage_nvs_classify_enc(enc)) {
    case BB_STORAGE_NVS_KIND_STR: {
        if (len + 1 > BB_STORAGE_NVS_GET_SCRATCH_MAX) {
            return BB_ERR_NO_SPACE;
        }
        char scratch[BB_STORAGE_NVS_GET_SCRATCH_MAX];
        if (len > 0) {
            memcpy(scratch, buf, len);
        }
        scratch[len] = '\0';
        return bb_storage_nvs_set_str(addr->ns_or_dir, addr->key, scratch);
    }

    case BB_STORAGE_NVS_KIND_U8:
        if (len != sizeof(uint8_t)) return BB_ERR_INVALID_ARG;
        return bb_storage_nvs_set_u8(addr->ns_or_dir, addr->key, ((const uint8_t *)buf)[0]);

    case BB_STORAGE_NVS_KIND_U16:
        if (len != sizeof(uint16_t)) return BB_ERR_INVALID_ARG;
        return bb_storage_nvs_set_u16(addr->ns_or_dir, addr->key, bb_load_le16(buf));

    case BB_STORAGE_NVS_KIND_U32:
        if (len != sizeof(uint32_t)) return BB_ERR_INVALID_ARG;
        return bb_storage_nvs_set_u32(addr->ns_or_dir, addr->key, bb_load_le32(buf));

    case BB_STORAGE_NVS_KIND_I32: {
        if (len != sizeof(uint32_t)) return BB_ERR_INVALID_ARG;
        uint32_t bits = bb_load_le32(buf);
        int32_t value;
        memcpy(&value, &bits, sizeof(value));
        return bb_storage_nvs_set_i32(addr->ns_or_dir, addr->key, value);
    }

    case BB_STORAGE_NVS_KIND_BLOB:
    default:
        return nvs_vt_set(impl, addr, buf, len);
    }
}

// nvs_txn_begin/set/commit/abort (used by s_nvs_vtable below) and their
// NVS-primitive seam are defined earlier in this file, before the
// ESP_PLATFORM/host split — see the "Multi-key transactions" comment near
// the top.
static const bb_storage_vtable_t s_nvs_vtable = {
    .get        = nvs_vt_get,
    .set        = nvs_vt_set,
    .erase      = nvs_vt_erase,
    .exists     = nvs_vt_exists,
    .get_typed  = nvs_vt_get_typed,
    .set_typed  = nvs_vt_set_typed,
    .txn_begin  = nvs_txn_begin,
    .txn_set    = nvs_txn_set,
    .txn_commit = nvs_txn_commit,
    .txn_abort  = nvs_txn_abort,
};

bb_err_t bb_storage_nvs_register(void)
{
    bb_err_t err = bb_storage_nvs_flash_init();
    if (err != BB_OK) {
        return err;
    }
    return bb_storage_register_backend("nvs", &s_nvs_vtable, NULL);
}

#else /* !ESP_PLATFORM — host stubs, never called by bb_nv's host build */

bb_err_t bb_storage_nvs_register(void)
{
    return BB_ERR_UNSUPPORTED;
}

bb_err_t bb_storage_nvs_set_u8(const char *ns, const char *key, uint8_t value)
{
    (void)value;
    if (ns == NULL || key == NULL) return BB_ERR_INVALID_ARG;
    return BB_ERR_UNSUPPORTED;
}

bb_err_t bb_storage_nvs_set_u16(const char *ns, const char *key, uint16_t value)
{
    (void)value;
    if (ns == NULL || key == NULL) return BB_ERR_INVALID_ARG;
    return BB_ERR_UNSUPPORTED;
}

bb_err_t bb_storage_nvs_set_u32(const char *ns, const char *key, uint32_t value)
{
    (void)value;
    if (ns == NULL || key == NULL) return BB_ERR_INVALID_ARG;
    return BB_ERR_UNSUPPORTED;
}

bb_err_t bb_storage_nvs_set_i32(const char *ns, const char *key, int32_t value)
{
    (void)value;
    if (ns == NULL || key == NULL) return BB_ERR_INVALID_ARG;
    return BB_ERR_UNSUPPORTED;
}

bb_err_t bb_storage_nvs_set_str(const char *ns, const char *key, const char *value)
{
    if (ns == NULL || key == NULL || value == NULL) return BB_ERR_INVALID_ARG;
    return BB_ERR_UNSUPPORTED;
}

bb_err_t bb_storage_nvs_get_u8(const char *ns, const char *key, uint8_t *out, uint8_t fallback)
{
    if (ns == NULL || key == NULL || out == NULL) return BB_ERR_INVALID_ARG;
    *out = fallback;
    return BB_ERR_UNSUPPORTED;
}

bb_err_t bb_storage_nvs_get_u16(const char *ns, const char *key, uint16_t *out, uint16_t fallback)
{
    if (ns == NULL || key == NULL || out == NULL) return BB_ERR_INVALID_ARG;
    *out = fallback;
    return BB_ERR_UNSUPPORTED;
}

bb_err_t bb_storage_nvs_get_u32(const char *ns, const char *key, uint32_t *out, uint32_t fallback)
{
    if (ns == NULL || key == NULL || out == NULL) return BB_ERR_INVALID_ARG;
    *out = fallback;
    return BB_ERR_UNSUPPORTED;
}

bb_err_t bb_storage_nvs_get_i32(const char *ns, const char *key, int32_t *out, int32_t fallback)
{
    if (ns == NULL || key == NULL || out == NULL) return BB_ERR_INVALID_ARG;
    *out = fallback;
    return BB_ERR_UNSUPPORTED;
}

bb_err_t bb_storage_nvs_get_str(const char *ns, const char *key, char *buf, size_t len, const char *fallback)
{
    if (ns == NULL || key == NULL || buf == NULL || len == 0) return BB_ERR_INVALID_ARG;
    (void)fallback;
    buf[0] = '\0';
    return BB_ERR_UNSUPPORTED;
}

bb_err_t bb_storage_nvs_erase(const char *ns, const char *key)
{
    if (ns == NULL || key == NULL) return BB_ERR_INVALID_ARG;
    return BB_ERR_UNSUPPORTED;
}

bb_err_t bb_storage_nvs_erase_namespace(const char *ns)
{
    if (ns == NULL) return BB_ERR_INVALID_ARG;
    return BB_ERR_UNSUPPORTED;
}

bool bb_storage_nvs_exists(const char *ns, const char *key)
{
    (void)ns; (void)key;
    return false;
}

#endif /* ESP_PLATFORM */
