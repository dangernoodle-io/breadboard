#include "test_alloc_inject.h"
#include <stdlib.h>

int test_alloc_fail_at = -1;
int test_alloc_call_count = 0;

void test_alloc_reset(void) {
    test_alloc_call_count = 0;
}

void *test_failing_calloc(size_t n, size_t sz) {
    if (test_alloc_call_count == test_alloc_fail_at) {
        test_alloc_call_count++;
        return NULL;
    }
    test_alloc_call_count++;
    return calloc(n, sz);
}

void *test_failing_malloc(size_t sz) {
    if (test_alloc_call_count == test_alloc_fail_at) {
        test_alloc_call_count++;
        return NULL;
    }
    test_alloc_call_count++;
    return malloc(sz);
}
