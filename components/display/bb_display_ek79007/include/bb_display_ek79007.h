#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "bb_core.h"

#ifdef ESP_PLATFORM
#include "lvgl.h"

/* Active LVGL screen for the EK79007 backend. NULL if not initialized.
 * All LVGL access from app code (including lv_timer callbacks not on
 * the LVGL task) MUST be wrapped in lock/unlock — the LVGL port owns
 * the only writer task, and concurrent access races. */
lv_obj_t *bb_display_ek79007_screen(void);

bool bb_display_ek79007_lock(uint32_t timeout_ms);  /* 0 = wait forever */
void bb_display_ek79007_unlock(void);

#endif /* ESP_PLATFORM */
