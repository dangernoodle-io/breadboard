#pragma once
// bb_lock — host backend private helper: raw pthread_mutex_t accessor shared
// between bb_lock.c (the mutex API) and bb_lock_cond.c (the condvar API,
// which must lock/unlock the SAME underlying pthread_mutex_t a caller passes
// as the paired bb_lock_t*). Not a public header — lives outside include/.

#include "bb_lock.h"
#include <pthread.h>

static inline pthread_mutex_t *bb_lock_impl(bb_lock_t *lock)
{
    return (pthread_mutex_t *)(void *)lock->bb_lock_impl.bb_lock_bytes;
}
