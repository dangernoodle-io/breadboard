#pragma once
// bb_storage_nvs_classify_nvs_type — pure NVS-raw-type-code -> bb_storage_enc_t
// mapping, extracted so the PR6 enumeration orchestration's type mapping is
// host-testable without nvs.h (mirrors bb_storage_nvs_classify_enc.h's
// shape/rationale — pure fn, no ESP-IDF types, one branch per case).
//
// The raw type codes accepted here are ESP-IDF's real nvs_type_t values
// (nvs.h) passed through as plain int — the enumeration orchestration takes
// whatever the injected bb_storage_nvs_entry_ops_t.info() callback reports,
// which on-device is a literal (int)nvs_type_t and in a host test is
// whatever fake value a test wants to exercise. Values below are copied from
// ESP-IDF's nvs.h (NVS_TYPE_* enumerators) rather than included from it, so
// this header carries no ESP-IDF dependency.
//
// bb_storage_enc_t has no representation for I8/I16/U64/I64/FLOAT/DOUBLE —
// those (and any unrecognized code) fall back to BB_STORAGE_ENC_BLOB, per
// PR6's "unrepresentable NVS types map to BLOB rather than erroring"
// contract.

#include "bb_storage.h"

#ifdef __cplusplus
extern "C" {
#endif

// Mirrors ESP-IDF nvs.h's nvs_type_t values (copied, not included, to keep
// this header ESP-IDF-free).
#define BB_STORAGE_NVS_RAW_TYPE_U8   0x01
#define BB_STORAGE_NVS_RAW_TYPE_I8   0x11
#define BB_STORAGE_NVS_RAW_TYPE_U16  0x02
#define BB_STORAGE_NVS_RAW_TYPE_I16  0x12
#define BB_STORAGE_NVS_RAW_TYPE_U32  0x04
#define BB_STORAGE_NVS_RAW_TYPE_I32  0x14
#define BB_STORAGE_NVS_RAW_TYPE_U64  0x08
#define BB_STORAGE_NVS_RAW_TYPE_I64  0x18
#define BB_STORAGE_NVS_RAW_TYPE_FLOAT  0x24
#define BB_STORAGE_NVS_RAW_TYPE_DOUBLE 0x28
#define BB_STORAGE_NVS_RAW_TYPE_STR  0x21
#define BB_STORAGE_NVS_RAW_TYPE_BLOB 0x42
#define BB_STORAGE_NVS_RAW_TYPE_ANY  0xff

// Pure raw-type-code -> bb_storage_enc_t mapping. Never fails; any code with
// no bb_storage_enc_t representation (including an unrecognized value)
// defaults to BB_STORAGE_ENC_BLOB.
bb_storage_enc_t bb_storage_nvs_classify_nvs_type(int raw_type);

#ifdef __cplusplus
}
#endif
