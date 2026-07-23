#include "bb_storage_nvs_classify_enc.h"

bb_storage_nvs_kind_t bb_storage_nvs_classify_enc(bb_storage_enc_t enc)
{
    switch (enc) {
    case BB_STORAGE_ENC_STR: return BB_STORAGE_NVS_KIND_STR;
    case BB_STORAGE_ENC_U8:  return BB_STORAGE_NVS_KIND_U8;
    case BB_STORAGE_ENC_U16: return BB_STORAGE_NVS_KIND_U16;
    case BB_STORAGE_ENC_U32: return BB_STORAGE_NVS_KIND_U32;
    case BB_STORAGE_ENC_I32: return BB_STORAGE_NVS_KIND_I32;
    case BB_STORAGE_ENC_BLOB:
    default:
        return BB_STORAGE_NVS_KIND_BLOB;
    }
}
