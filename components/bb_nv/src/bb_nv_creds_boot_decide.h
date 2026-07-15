#pragma once
// bb_nv_creds_boot_decide — pure decision logic for bb_nv_config_init's
// heal-vs-seed boot policy, extracted from platform/espidf/bb_nv/bb_nv.c so
// Coveralls sees every branch and the host test suite exercises the
// highest-risk decision (which one strands a board if inverted) without
// NVS/RTC hardware. No ESP-IDF/NVS types — host-testable in isolation,
// mirroring bb_diag_reset_decision.c / bb_storage_nvs_get_decision.h.
//
// The actual NVS read/write + RTC mirror I/O stays in bb_nv_config_init's
// #ifdef ESP_PLATFORM shell (B1-943/B1-516, espidf-only, coverage-invisible,
// rides on HW validation) — this header/impl covers ONLY which action to
// take, not the I/O itself.

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BB_NV_BOOT_NONE = 0,  // nothing to do: state already consistent
    BB_NV_BOOT_HEAL,      // NVS has no live creds but the RTC mirror does -- restore
    BB_NV_BOOT_SEED,      // live creds exist but the RTC mirror is empty/invalid -- arm it
} bb_nv_boot_action_t;

// Pure state -> action mapping, matching the two conditions inlined in
// bb_nv_config_init (platform/espidf/bb_nv/bb_nv.c):
//   - !live_creds_present && mirror_valid_with_creds  -> HEAL
//   - live_creds_present && !mirror_valid_with_creds  -> SEED
//   - anything else (both true or both false)          -> NONE
// Heal takes precedence in the caller's I/O ordering (decided/executed
// first) so an NVS-erased+valid-mirror boot restores before any seed
// consideration -- moot for this function's own logic since the two
// conditions above are mutually exclusive by construction, but the caller's
// sequencing still matters for its own post-heal re-check semantics.
bb_nv_boot_action_t bb_nv_creds_boot_decide(bool live_creds_present, bool mirror_valid_with_creds);

#ifdef __cplusplus
}
#endif
