#include "bb_settings_creds_boot_decide.h"

bb_settings_creds_boot_action_t bb_settings_creds_boot_decide(bool live_creds_present,
                                                                bool mirror_valid_with_creds)
{
    if (!live_creds_present && mirror_valid_with_creds) {
        return BB_SETTINGS_CREDS_BOOT_HEAL;
    }
    if (live_creds_present && !mirror_valid_with_creds) {
        return BB_SETTINGS_CREDS_BOOT_SEED;
    }
    return BB_SETTINGS_CREDS_BOOT_NONE;
}
