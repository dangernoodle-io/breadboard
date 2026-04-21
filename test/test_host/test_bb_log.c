#include "unity.h"
#include "bb_log.h"

void test_bb_log_error(void) {
    bb_log_e("TAG", "msg %d", 1);
    TEST_PASS();
}

void test_bb_log_warning(void) {
    bb_log_w("TAG", "msg %d", 1);
    TEST_PASS();
}

void test_bb_log_info(void) {
    bb_log_i("TAG", "msg %d", 1);
    TEST_PASS();
}

void test_bb_log_debug(void) {
    bb_log_d("TAG", "msg %d", 1);
    TEST_PASS();
}

void test_bb_log_verbose(void) {
    bb_log_v("TAG", "msg %d", 1);
    TEST_PASS();
}

void test_bb_log_zero_args(void) {
    bb_log_e("TAG", "literal");
    TEST_PASS();
}
