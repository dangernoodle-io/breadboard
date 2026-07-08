#include "bb_storage_nvs_get_decision.h"

bb_storage_nvs_get_outcome_t bb_storage_nvs_get_decide(size_t required, size_t cap,
                                                        size_t scratch_max, size_t reserve,
                                                        size_t *out_len)
{
    if (out_len != NULL) {
        *out_len = required;
    }

    if (cap == 0) {
        return BB_STORAGE_NVS_GET_PROBE;
    }
    if (cap >= required + reserve) {
        return BB_STORAGE_NVS_GET_FULL;
    }
    if (required + reserve <= scratch_max) {
        return BB_STORAGE_NVS_GET_BOUNCE;
    }
    return BB_STORAGE_NVS_GET_NO_SPACE;
}
