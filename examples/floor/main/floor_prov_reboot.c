// floor_prov_reboot — see floor_prov_reboot.h for the full contract.

#include "floor_prov_reboot.h"

bool floor_should_trigger_prov_reboot(bool prov_mode_boot, bool already_triggered,
                                       bb_lifecycle_svc_t evt_svc, bb_lifecycle_svc_t wifi_svc,
                                       bb_lifecycle_state_t old_state, bb_lifecycle_state_t new_state)
{
    if (evt_svc != wifi_svc) {
        return false;
    }
    if (!prov_mode_boot || already_triggered) {
        return false;
    }
    if (new_state != BB_LIFECYCLE_RUNNING || old_state == BB_LIFECYCLE_RUNNING) {
        return false;
    }
    return true;
}
