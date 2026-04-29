#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool started;
    bool announce_dirty;
} bb_mdns_lifecycle_state_t;

typedef struct {
    int  (*mdns_init)(void);             /* 0 on success */
    void (*mdns_free)(void);
    int  (*mdns_apply_announce)(void);   /* 0 on success */
    void (*mdns_send_bye)(void);         /* best-effort; no return */
} bb_mdns_lifecycle_adapter_t;

typedef enum {
    BB_MDNS_LC_OK,
    BB_MDNS_LC_ALREADY_STARTED,
    BB_MDNS_LC_NOT_STARTED,
    BB_MDNS_LC_INIT_FAILED,
    BB_MDNS_LC_INVALID_ARG,
} bb_mdns_lifecycle_result_t;

void                       bb_mdns_lifecycle_reset(bb_mdns_lifecycle_state_t *st);
bb_mdns_lifecycle_result_t bb_mdns_lifecycle_start(bb_mdns_lifecycle_state_t *st, const bb_mdns_lifecycle_adapter_t *a);
bb_mdns_lifecycle_result_t bb_mdns_lifecycle_stop(bb_mdns_lifecycle_state_t *st, const bb_mdns_lifecycle_adapter_t *a);
bb_mdns_lifecycle_result_t bb_mdns_lifecycle_announce(bb_mdns_lifecycle_state_t *st, const bb_mdns_lifecycle_adapter_t *a);
void                       bb_mdns_lifecycle_mark_dirty(bb_mdns_lifecycle_state_t *st);
bool                       bb_mdns_lifecycle_is_started(const bb_mdns_lifecycle_state_t *st);
