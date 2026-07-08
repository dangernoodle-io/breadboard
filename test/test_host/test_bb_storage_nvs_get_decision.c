#include "unity.h"
#include "bb_storage_nvs_get_decision.h"

// PR2 MED finding: nvs_vt_get's four read-outcome branches, extracted into a
// pure host-testable decision function so Coveralls sees every branch
// without requiring NVS. See components/bb_storage_nvs/src/
// bb_storage_nvs_get_decision.h for the outcome contract.

void test_bb_storage_nvs_get_decide_zero_cap_probes(void)
{
    size_t out_len = 0;
    TEST_ASSERT_EQUAL(BB_STORAGE_NVS_GET_PROBE, bb_storage_nvs_get_decide(10, 0, 512, 0, &out_len));
    TEST_ASSERT_EQUAL(10, out_len);
}

void test_bb_storage_nvs_get_decide_cap_equal_required_is_full(void)
{
    size_t out_len = 0;
    TEST_ASSERT_EQUAL(BB_STORAGE_NVS_GET_FULL, bb_storage_nvs_get_decide(10, 10, 512, 0, &out_len));
    TEST_ASSERT_EQUAL(10, out_len);
}

void test_bb_storage_nvs_get_decide_cap_greater_than_required_is_full(void)
{
    size_t out_len = 0;
    TEST_ASSERT_EQUAL(BB_STORAGE_NVS_GET_FULL, bb_storage_nvs_get_decide(10, 16, 512, 0, &out_len));
    TEST_ASSERT_EQUAL(10, out_len);
}

void test_bb_storage_nvs_get_decide_truncating_within_scratch_bounces(void)
{
    size_t out_len = 0;
    TEST_ASSERT_EQUAL(BB_STORAGE_NVS_GET_BOUNCE, bb_storage_nvs_get_decide(100, 4, 512, 0, &out_len));
    TEST_ASSERT_EQUAL(100, out_len);
}

void test_bb_storage_nvs_get_decide_truncating_at_scratch_limit_bounces(void)
{
    size_t out_len = 0;
    TEST_ASSERT_EQUAL(BB_STORAGE_NVS_GET_BOUNCE, bb_storage_nvs_get_decide(512, 4, 512, 0, &out_len));
    TEST_ASSERT_EQUAL(512, out_len);
}

void test_bb_storage_nvs_get_decide_truncating_beyond_scratch_is_no_space(void)
{
    size_t out_len = 0;
    TEST_ASSERT_EQUAL(BB_STORAGE_NVS_GET_NO_SPACE, bb_storage_nvs_get_decide(513, 4, 512, 0, &out_len));
    TEST_ASSERT_EQUAL(513, out_len);
}

void test_bb_storage_nvs_get_decide_zero_required_zero_cap_probes(void)
{
    size_t out_len = 123;
    TEST_ASSERT_EQUAL(BB_STORAGE_NVS_GET_PROBE, bb_storage_nvs_get_decide(0, 0, 512, 0, &out_len));
    TEST_ASSERT_EQUAL(0, out_len);
}

void test_bb_storage_nvs_get_decide_zero_required_nonzero_cap_is_full(void)
{
    size_t out_len = 123;
    TEST_ASSERT_EQUAL(BB_STORAGE_NVS_GET_FULL, bb_storage_nvs_get_decide(0, 4, 512, 0, &out_len));
    TEST_ASSERT_EQUAL(0, out_len);
}

void test_bb_storage_nvs_get_decide_null_out_len_is_safe(void)
{
    TEST_ASSERT_EQUAL(BB_STORAGE_NVS_GET_PROBE, bb_storage_nvs_get_decide(10, 0, 512, 0, NULL));
}

// --- reserve=1 (NUL-inclusive) boundary tests, HIGH finding fix (nvs_vt_get_typed_str) ---
// nvs_get_str requires cap to cover the string bytes PLUS the NUL. A caller
// with cap == str_len exactly (the natural PROBE-then-alloc-exactly-str_len
// retry pattern) must NOT be routed to FULL — it has no room for the NUL.

void test_bb_storage_nvs_get_decide_reserve1_cap_equal_required_is_bounce(void)
{
    // cap == str_len (no room for NUL): must bounce, not FULL.
    size_t out_len = 0;
    TEST_ASSERT_EQUAL(BB_STORAGE_NVS_GET_BOUNCE, bb_storage_nvs_get_decide(10, 10, 512, 1, &out_len));
    TEST_ASSERT_EQUAL(10, out_len);
}

void test_bb_storage_nvs_get_decide_reserve1_cap_equal_required_plus_one_is_full(void)
{
    // cap == str_len + 1 (room for the NUL): safe to read straight into buf.
    size_t out_len = 0;
    TEST_ASSERT_EQUAL(BB_STORAGE_NVS_GET_FULL, bb_storage_nvs_get_decide(10, 11, 512, 1, &out_len));
    TEST_ASSERT_EQUAL(10, out_len);
}

void test_bb_storage_nvs_get_decide_reserve1_probed_equal_scratch_max_bounces(void)
{
    // required + reserve == scratch_max exactly: still fits, BOUNCE.
    size_t out_len = 0;
    TEST_ASSERT_EQUAL(BB_STORAGE_NVS_GET_BOUNCE, bb_storage_nvs_get_decide(511, 4, 512, 1, &out_len));
    TEST_ASSERT_EQUAL(511, out_len);
}

void test_bb_storage_nvs_get_decide_reserve1_probed_exceeds_scratch_max_is_no_space(void)
{
    // required + reserve > scratch_max: cannot stage, NO_SPACE.
    size_t out_len = 0;
    TEST_ASSERT_EQUAL(BB_STORAGE_NVS_GET_NO_SPACE, bb_storage_nvs_get_decide(512, 4, 512, 1, &out_len));
    TEST_ASSERT_EQUAL(512, out_len);
}
