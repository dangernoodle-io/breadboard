#pragma once
// Portable typed lock-copy helper — POSIX pthread (host) and ESP-IDF POSIX layer.
#include <pthread.h>

// BB_LOCKED_COPY — acquire mtx_ptr, copy src to dst via typed struct assignment, release.
// Only for single-assignment critical sections; do not use when multiple operations
// must run atomically under the same lock.
#define BB_LOCKED_COPY(mtx_ptr, dst, src) do { \
    pthread_mutex_lock(mtx_ptr);                \
    (dst) = (src);                              \
    pthread_mutex_unlock(mtx_ptr);              \
} while (0)
