#pragma once

#include <stddef.h>

/* Counts allocations; fails the N-th one (0-indexed). */
extern int test_alloc_fail_at;     /* -1 = never fail */
extern int test_alloc_call_count;
void test_alloc_reset(void);
void *test_failing_calloc(size_t n, size_t sz);
void *test_failing_malloc(size_t sz);
