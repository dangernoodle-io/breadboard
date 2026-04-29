#pragma once
#include <stdint.h>
#include "bb_core.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void *bb_timer_handle_t;
typedef void (*bb_timer_cb_t)(void *arg);

typedef enum {
    BB_TIMER_ONE_SHOT,
    BB_TIMER_PERIODIC,
} bb_timer_type_t;

bb_err_t bb_timer_create(const char *name, bb_timer_type_t type,
                         uint64_t period_us, bb_timer_cb_t cb, void *arg,
                         bb_timer_handle_t *out);
bb_err_t bb_timer_start(bb_timer_handle_t h);
bb_err_t bb_timer_stop(bb_timer_handle_t h);
bb_err_t bb_timer_delete(bb_timer_handle_t h);

#ifdef __cplusplus
}
#endif
