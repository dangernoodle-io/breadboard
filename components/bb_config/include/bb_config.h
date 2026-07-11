#pragma once

// bb_config — typed configuration layer over bb_storage.
//
// bb_storage's vtable is blob-only (get/set raw bytes at an addr); bb_config
// is the layer that gives it scalar-typed meaning. A consumer declares a
// static const bb_config_field_t table describing each field's type, storage
// address, and (for STR/BLOB) capacity, then calls the matching typed
// accessor. Scalars are byte-encoded via bb_core's bb_byte_order helpers
// (fixed-width little-endian); bool is a single 0x00/0x01 byte.
//
// No global registry, no init function: fields are caller-owned `static
// const` data (typically .rodata) — accessors resolve `f->addr` against
// bb_storage per call. Nothing here self-registers or holds state, so this
// component is composition-only by construction (see the DI legacy fence in
// breadboard/CLAUDE.md).
//
// Concurrency is inherited entirely from the backend a field's addr targets
// (e.g. NVS serializes internally). bb_config adds no per-field lock of its
// own — a consumer needing atomic read-modify-write across multiple fields
// must arrange its own synchronization.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bb_core.h"
#include "bb_storage.h"

#ifdef __cplusplus
extern "C" {
#endif

// Field value type.
typedef enum {
    BB_CONFIG_BOOL,
    BB_CONFIG_U8,
    BB_CONFIG_U16,
    BB_CONFIG_U32,
    BB_CONFIG_I32,
    BB_CONFIG_STR,
    BB_CONFIG_BLOB,
} bb_config_type_t;

// Default value for a scalar field. STR/BLOB fields have no representable
// default here (BLOB has no natural "default bytes"; STR defaults are rare
// enough that a NULL-terminated .str member covers the sole known use case).
typedef union {
    bool        b;
    uint8_t     u8;
    uint16_t    u16;
    uint32_t    u32;
    int32_t     i32;
    const char *str;
} bb_config_default_t;

// Rich field descriptor. Caller-owned, typically declared `static const`.
// `id` is a stable field identifier (e.g. "wifi.ssid") — distinct from the
// underlying storage key carried in `addr`. `max_len` applies to STR/BLOB
// only (0 for scalar types). The render-metadata fields (label/help/group/
// secret/provisioning_only/reboot_required) are carried but unconsumed in
// this PR — reserved for a later schema-endpoint consumer.
typedef struct {
    const char         *id;
    bb_config_type_t    type;
    bb_storage_addr_t   addr;
    size_t              max_len;
    bb_config_default_t def;
    bool                has_default;

    // Provisioning-form render metadata (carried, not yet consumed).
    const char *label;
    const char *help;
    const char *group;
    bool        secret;
    bool        provisioning_only;
    bool        reboot_required;
} bb_config_field_t;

// Pure, stateless type-metadata helpers -- no storage access, no atomicity
// implications (see the "no multi-field atomicity" note above).

// Maps a bb_config_type_t to the bb_storage_enc_t used to round-trip it
// through a typed backend (e.g. native NVS entries). See bb_config.c for the
// full mapping rationale.
bb_storage_enc_t bb_config_type_to_enc(bb_config_type_t t);

// Fixed encoded byte width for scalar types (BOOL/U8=1, U16=2, U32/I32=4).
// Returns 0 for STR/BLOB, which are variable-length.
size_t bb_config_scalar_width(bb_config_type_t t);

// Typed accessors. Each validates f->type against the call — a mismatch
// returns BB_ERR_INVALID_ARG without touching storage. On BB_ERR_NOT_FOUND
// from the backend, a field with has_default=true resolves to f->def and
// returns BB_OK instead of propagating the not-found error.
//
// Common return codes across all get/set accessors:
//   BB_OK                 success (see per-accessor notes)
//   BB_ERR_INVALID_ARG    f is NULL, f->type doesn't match the call, or (set)
//                         invalid value (e.g. string too long)
//   BB_ERR_NOT_FOUND      no value stored and has_default is false
//   BB_ERR_INVALID_STATE  (scalar get only) the stored value's length does
//                         not match the accessor's fixed width -- e.g. a
//                         short/foreign/corrupt write under the same addr
//   (other bb_storage_get/set error codes pass through unchanged)
bb_err_t bb_config_get_bool(const bb_config_field_t *f, bool *out);
bb_err_t bb_config_set_bool(const bb_config_field_t *f, bool v);

bb_err_t bb_config_get_u8(const bb_config_field_t *f, uint8_t *out);
bb_err_t bb_config_set_u8(const bb_config_field_t *f, uint8_t v);

bb_err_t bb_config_get_u16(const bb_config_field_t *f, uint16_t *out);
bb_err_t bb_config_set_u16(const bb_config_field_t *f, uint16_t v);

bb_err_t bb_config_get_u32(const bb_config_field_t *f, uint32_t *out);
bb_err_t bb_config_set_u32(const bb_config_field_t *f, uint32_t v);

bb_err_t bb_config_get_i32(const bb_config_field_t *f, int32_t *out);
bb_err_t bb_config_set_i32(const bb_config_field_t *f, int32_t v);

// Reads the stored string into buf (capacity cap), writing the actual stored
// length to *out_len regardless of whether it fit — same truncation contract
// as bb_storage_get() (callers detect truncation via *out_len > cap; cap=0
// is a valid size-probe). On not-found with has_default=true, f->def.str is
// copied into buf under the same cap/out_len contract.
bb_err_t bb_config_get_str(const bb_config_field_t *f, char *buf, size_t cap, size_t *out_len);

// Writes v as the stored string. Returns BB_ERR_INVALID_ARG if
// strlen(v) >= f->max_len (the stored value must always fit with room for a
// NUL terminator on read-back).
bb_err_t bb_config_set_str(const bb_config_field_t *f, const char *v);

// Blob get/set. Same truncation/size-probe contract as bb_config_get_str.
// BLOB fields have no default fallback (has_default is ignored for BLOB).
bb_err_t bb_config_get_blob(const bb_config_field_t *f, void *buf, size_t cap, size_t *out_len);
bb_err_t bb_config_set_blob(const bb_config_field_t *f, const void *v, size_t len);

// Erase the stored value at f->addr, if any. Idempotent (erasing an absent
// value is BB_OK). Not type-checked — erase is type-agnostic.
bb_err_t bb_config_erase(const bb_config_field_t *f);

// True iff a value is currently stored at f->addr. false for a NULL f.
bool bb_config_exists(const bb_config_field_t *f);

#ifdef __cplusplus
}
#endif
