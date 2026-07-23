#pragma once
// bb_storage_nvs_classify_enc — pure bb_storage_enc_t -> NVS-call-kind
// mapping, extracted from nvs_vt_get_typed/nvs_vt_set_typed's dispatch
// switch so Coveralls sees every enc branch and the host test suite
// exercises the mapping without NVS. No ESP-IDF/NVS types — host-testable
// in isolation, mirroring bb_storage_nvs_get_decision.h.
//
// Every bb_storage_enc_t value maps to exactly one kind; an out-of-range
// value (defensive — enums are not bounds-checked in C) defaults to BLOB,
// matching bb_storage_enc_t's own BB_STORAGE_ENC_BLOB = 0 default.

#include "bb_storage.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BB_STORAGE_NVS_KIND_BLOB = 0,  // nvs_get/set_blob
    BB_STORAGE_NVS_KIND_STR,       // nvs_get/set_str
    BB_STORAGE_NVS_KIND_U8,        // nvs_get/set_u8
    BB_STORAGE_NVS_KIND_U16,       // nvs_get/set_u16
    BB_STORAGE_NVS_KIND_U32,       // nvs_get/set_u32
    BB_STORAGE_NVS_KIND_I32,       // nvs_get/set_i32
} bb_storage_nvs_kind_t;

// Pure enc -> kind mapping. Never fails; unrecognized enc values fall back
// to BLOB (the safe, always-correct-for-any-bytes encoding).
bb_storage_nvs_kind_t bb_storage_nvs_classify_enc(bb_storage_enc_t enc);

#ifdef __cplusplus
}
#endif
