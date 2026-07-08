#include "bb_storage_nvs.h"
#include "bb_storage.h"

#include <string.h>

#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "bb_log.h"
#include "bb_str.h"
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
        bb_storage_nvs_get_decide(required, cap, BB_STORAGE_NVS_GET_SCRATCH_MAX, out_len);

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

static const bb_storage_vtable_t s_nvs_vtable = {
    .get    = nvs_vt_get,
    .set    = nvs_vt_set,
    .erase  = nvs_vt_erase,
    .exists = nvs_vt_exists,
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
