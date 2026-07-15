#include "bb_nv_creds_boot_decide.h"

bb_nv_boot_action_t bb_nv_creds_boot_decide(bool live_creds_present, bool mirror_valid_with_creds)
{
    if (!live_creds_present && mirror_valid_with_creds) {
        return BB_NV_BOOT_HEAL;
    }
    if (live_creds_present && !mirror_valid_with_creds) {
        return BB_NV_BOOT_SEED;
    }
    return BB_NV_BOOT_NONE;
}
