#include "unity.h"
#include "bb_nv.h"

// bb_nv_config_is_provisioned is NOT part of B1-963's creds-boot relocation
// (zero external callers, staying dead pending bb_nv's full deletion,
// B1-964) -- this is the only surviving test from this file; the
// init/manifest tests moved to test_bb_settings_creds_boot.c alongside the
// bb_settings functions they now exercise.
void test_nv_config_is_provisioned_stub_returns_false(void)
{
    bool provisioned = bb_nv_config_is_provisioned();
    TEST_ASSERT_FALSE(provisioned);
}
