// bb_config — shared portable implementation (compiled host + espidf +
// arduino). No platform includes; only bb_storage + bb_byte_order (bb_core).

#include "bb_config.h"

#include <string.h>

#include "bb_byte_order.h"

// ---------------------------------------------------------------------------
// bb_config_type_t -> bb_storage_enc_t — lets NVS-backed fields round-trip
// through native NVS typed entries (nvs_set_u8/u16/u32/i32/str) instead of
// an untyped blob, preserving on-flash type tags for provisioned boards.
// Backends without a get_typed/set_typed pair (RAM/host/sdcard) ignore the
// encoding via bb_storage_get_typed/set_typed's blob fallback, so this
// mapping is a no-op there.
// ---------------------------------------------------------------------------

bb_storage_enc_t bb_config_type_to_enc(bb_config_type_t t)
{
    switch (t) {
    case BB_CONFIG_BOOL:
    case BB_CONFIG_U8:
        return BB_STORAGE_ENC_U8;
    case BB_CONFIG_U16:
        return BB_STORAGE_ENC_U16;
    case BB_CONFIG_U32:
        return BB_STORAGE_ENC_U32;
    case BB_CONFIG_I32:
        return BB_STORAGE_ENC_I32;
    case BB_CONFIG_STR:
        return BB_STORAGE_ENC_STR;
    case BB_CONFIG_BLOB:
    default:
        return BB_STORAGE_ENC_BLOB;
    }
}

size_t bb_config_scalar_width(bb_config_type_t t)
{
    switch (t) {
    case BB_CONFIG_BOOL:
    case BB_CONFIG_U8:
        return 1;
    case BB_CONFIG_U16:
        return 2;
    case BB_CONFIG_U32:
    case BB_CONFIG_I32:
        return 4;
    case BB_CONFIG_STR:
    case BB_CONFIG_BLOB:
    default:
        return 0;
    }
}

// ---------------------------------------------------------------------------
// Scalar helpers
// ---------------------------------------------------------------------------

static bb_err_t scalar_get(const bb_config_field_t *f, bb_config_type_t want, uint8_t *buf)
{
    if (f == NULL || f->type != want) {
        return BB_ERR_INVALID_ARG;
    }

    size_t width = bb_config_scalar_width(want);
    memset(buf, 0, width);

    size_t out_len = 0;
    bb_err_t rc = bb_storage_get_typed(&f->addr, bb_config_type_to_enc(f->type), buf, width, &out_len);
    if (rc != BB_OK) {
        return rc;
    }
    if (out_len != width) {
        // Stored value is shorter than this accessor's fixed width (type
        // migrated without erase, aliasing addr, or corrupt/foreign write).
        // Returning BB_OK here would hand back a scratch-initialized value
        // that looks valid but isn't -- fail loudly instead.
        return BB_ERR_INVALID_STATE;
    }
    return BB_OK;
}

static bb_err_t scalar_set(const bb_config_field_t *f, bb_config_type_t want, const uint8_t *buf)
{
    if (f == NULL || f->type != want) {
        return BB_ERR_INVALID_ARG;
    }
    return bb_storage_set_typed(&f->addr, bb_config_type_to_enc(f->type), buf, bb_config_scalar_width(want));
}

// ---------------------------------------------------------------------------
// bool — single 0x00/0x01 byte
// ---------------------------------------------------------------------------

bb_err_t bb_config_get_bool(const bb_config_field_t *f, bool *out)
{
    if (out == NULL) {
        return BB_ERR_INVALID_ARG;
    }
    uint8_t byte = 0;
    bb_err_t rc = scalar_get(f, BB_CONFIG_BOOL, &byte);
    if (rc == BB_ERR_NOT_FOUND && f->has_default) {
        *out = f->def.b;
        return BB_OK;
    }
    if (rc != BB_OK) {
        return rc;
    }
    *out = (byte != 0);
    return BB_OK;
}

bb_err_t bb_config_set_bool(const bb_config_field_t *f, bool v)
{
    uint8_t byte = v ? 1 : 0;
    return scalar_set(f, BB_CONFIG_BOOL, &byte);
}

// ---------------------------------------------------------------------------
// u8
// ---------------------------------------------------------------------------

bb_err_t bb_config_get_u8(const bb_config_field_t *f, uint8_t *out)
{
    if (out == NULL) {
        return BB_ERR_INVALID_ARG;
    }
    uint8_t byte = 0;
    bb_err_t rc = scalar_get(f, BB_CONFIG_U8, &byte);
    if (rc == BB_ERR_NOT_FOUND && f->has_default) {
        *out = f->def.u8;
        return BB_OK;
    }
    if (rc != BB_OK) {
        return rc;
    }
    *out = byte;
    return BB_OK;
}

bb_err_t bb_config_set_u8(const bb_config_field_t *f, uint8_t v)
{
    return scalar_set(f, BB_CONFIG_U8, &v);
}

// ---------------------------------------------------------------------------
// u16
// ---------------------------------------------------------------------------

bb_err_t bb_config_get_u16(const bb_config_field_t *f, uint16_t *out)
{
    if (out == NULL) {
        return BB_ERR_INVALID_ARG;
    }
    uint8_t buf[2];
    bb_err_t rc = scalar_get(f, BB_CONFIG_U16, buf);
    if (rc == BB_ERR_NOT_FOUND && f->has_default) {
        *out = f->def.u16;
        return BB_OK;
    }
    if (rc != BB_OK) {
        return rc;
    }
    *out = bb_load_le16(buf);
    return BB_OK;
}

bb_err_t bb_config_set_u16(const bb_config_field_t *f, uint16_t v)
{
    uint8_t buf[2];
    bb_store_le16(buf, v);
    return scalar_set(f, BB_CONFIG_U16, buf);
}

// ---------------------------------------------------------------------------
// u32
// ---------------------------------------------------------------------------

bb_err_t bb_config_get_u32(const bb_config_field_t *f, uint32_t *out)
{
    if (out == NULL) {
        return BB_ERR_INVALID_ARG;
    }
    uint8_t buf[4];
    bb_err_t rc = scalar_get(f, BB_CONFIG_U32, buf);
    if (rc == BB_ERR_NOT_FOUND && f->has_default) {
        *out = f->def.u32;
        return BB_OK;
    }
    if (rc != BB_OK) {
        return rc;
    }
    *out = bb_load_le32(buf);
    return BB_OK;
}

bb_err_t bb_config_set_u32(const bb_config_field_t *f, uint32_t v)
{
    uint8_t buf[4];
    bb_store_le32(buf, v);
    return scalar_set(f, BB_CONFIG_U32, buf);
}

// ---------------------------------------------------------------------------
// i32 — stored as its u32 bit pattern (two's complement, portable)
// ---------------------------------------------------------------------------

bb_err_t bb_config_get_i32(const bb_config_field_t *f, int32_t *out)
{
    if (out == NULL) {
        return BB_ERR_INVALID_ARG;
    }
    uint8_t buf[4];
    bb_err_t rc = scalar_get(f, BB_CONFIG_I32, buf);
    if (rc == BB_ERR_NOT_FOUND && f->has_default) {
        *out = f->def.i32;
        return BB_OK;
    }
    if (rc != BB_OK) {
        return rc;
    }
    uint32_t bits = bb_load_le32(buf);
    memcpy(out, &bits, sizeof(bits));
    return BB_OK;
}

bb_err_t bb_config_set_i32(const bb_config_field_t *f, int32_t v)
{
    uint32_t bits;
    memcpy(&bits, &v, sizeof(bits));
    uint8_t buf[4];
    bb_store_le32(buf, bits);
    return scalar_set(f, BB_CONFIG_I32, buf);
}

// ---------------------------------------------------------------------------
// str
// ---------------------------------------------------------------------------

bb_err_t bb_config_get_str(const bb_config_field_t *f, char *buf, size_t cap, size_t *out_len)
{
    if (f == NULL || f->type != BB_CONFIG_STR || out_len == NULL || (cap > 0 && buf == NULL)) {
        return BB_ERR_INVALID_ARG;
    }

    bb_err_t rc = bb_storage_get_typed(&f->addr, BB_STORAGE_ENC_STR, buf, cap, out_len);
    if (rc == BB_ERR_NOT_FOUND && f->has_default) {
        const char *def = f->def.str != NULL ? f->def.str : "";
        size_t len = strlen(def);
        if (len >= f->max_len) {
            // Static misconfiguration: the field's own default doesn't fit
            // its own max_len. Fail loudly rather than silently truncating.
            return BB_ERR_INVALID_ARG;
        }
        *out_len = len;
        if (cap > 0) {
            size_t n = len < cap ? len : cap;
            memcpy(buf, def, n);
        }
        return BB_OK;
    }
    return rc;
}

bb_err_t bb_config_set_str(const bb_config_field_t *f, const char *v)
{
    if (f == NULL || f->type != BB_CONFIG_STR || v == NULL) {
        return BB_ERR_INVALID_ARG;
    }
    size_t len = strlen(v);
    if (len >= f->max_len) {
        return BB_ERR_INVALID_ARG;
    }
    return bb_storage_set_typed(&f->addr, BB_STORAGE_ENC_STR, v, len);
}

// ---------------------------------------------------------------------------
// blob — no default fallback
// ---------------------------------------------------------------------------

bb_err_t bb_config_get_blob(const bb_config_field_t *f, void *buf, size_t cap, size_t *out_len)
{
    if (f == NULL || f->type != BB_CONFIG_BLOB || out_len == NULL || (cap > 0 && buf == NULL)) {
        return BB_ERR_INVALID_ARG;
    }
    return bb_storage_get_typed(&f->addr, BB_STORAGE_ENC_BLOB, buf, cap, out_len);
}

bb_err_t bb_config_set_blob(const bb_config_field_t *f, const void *v, size_t len)
{
    if (f == NULL || f->type != BB_CONFIG_BLOB || (len > 0 && v == NULL)) {
        return BB_ERR_INVALID_ARG;
    }
    if (len > f->max_len) {
        return BB_ERR_INVALID_ARG;
    }
    return bb_storage_set_typed(&f->addr, BB_STORAGE_ENC_BLOB, v, len);
}

// ---------------------------------------------------------------------------
// erase / exists — type-agnostic
// ---------------------------------------------------------------------------

bb_err_t bb_config_erase(const bb_config_field_t *f)
{
    if (f == NULL) {
        return BB_ERR_INVALID_ARG;
    }
    return bb_storage_erase(&f->addr);
}

bool bb_config_exists(const bb_config_field_t *f)
{
    if (f == NULL) {
        return false;
    }
    return bb_storage_exists(&f->addr);
}
