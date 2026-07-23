#include "bb_storage_nvs_classify_nvs_type.h"

bb_storage_enc_t bb_storage_nvs_classify_nvs_type(int raw_type)
{
    switch (raw_type) {
    case BB_STORAGE_NVS_RAW_TYPE_STR: return BB_STORAGE_ENC_STR;
    case BB_STORAGE_NVS_RAW_TYPE_U8:  return BB_STORAGE_ENC_U8;
    case BB_STORAGE_NVS_RAW_TYPE_U16: return BB_STORAGE_ENC_U16;
    case BB_STORAGE_NVS_RAW_TYPE_U32: return BB_STORAGE_ENC_U32;
    case BB_STORAGE_NVS_RAW_TYPE_I32: return BB_STORAGE_ENC_I32;
    case BB_STORAGE_NVS_RAW_TYPE_BLOB:
    default:
        return BB_STORAGE_ENC_BLOB;
    }
}
