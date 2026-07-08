#include "bb_storage_nvs.h"
#include "bb_storage.h"

#include <string.h>

#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "bb_log.h"
#include "bb_str.h"
#include "bb_byte_order.h"
#include "bb_storage_nvs_get_decision.h"
#include "bb_storage_nvs_classify_enc.h"

// ---------------------------------------------------------------------------
// Capacity constants (Kconfig bridge — pattern from bb_clock.h)
// ---------------------------------------------------------------------------
#ifdef CONFIG_BB_STORAGE_NVS_GET_SCRATCH_MAX
#define BB_STORAGE_NVS_GET_SCRATCH_MAX CONFIG_BB_STORAGE_NVS_GET_SCRATCH_MAX
#endif
#ifndef BB_STORAGE_NVS_GET_SCRATCH_MAX
#define BB_STORAGE_NVS_GET_SCRATCH_MAX 512
#endif

static const char *TAG = "bb_storage_nvs";

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
        memcpy(buf, scratch, cap < str_len ? cap : str_len);
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

static const bb_storage_vtable_t s_nvs_vtable = {
    .get       = nvs_vt_get,
    .set       = nvs_vt_set,
    .erase     = nvs_vt_erase,
    .exists    = nvs_vt_exists,
    .get_typed = nvs_vt_get_typed,
    .set_typed = nvs_vt_set_typed,
};

bb_err_t bb_storage_nvs_register(void)
{
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
