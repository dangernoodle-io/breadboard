#include "bb_ota_led.h"

static const bb_ota_led_ops_t *s_ops = NULL;
static void *s_ctx = NULL;

void bb_ota_led_init(const bb_ota_led_ops_t *ops, void *ctx)
{
    s_ops = ops;
    s_ctx = ctx;
}

void bb_ota_led_progress(bb_ota_phase_t phase, int pct)
{
    if (!s_ops) {
        return;
    }
    switch (phase) {
        case BB_OTA_PHASE_START:
        case BB_OTA_PHASE_PROGRESS:
            if (s_ops->updating) {
                s_ops->updating(s_ctx, pct);
            }
            break;
        case BB_OTA_PHASE_SUCCESS:
            if (s_ops->success) {
                s_ops->success(s_ctx);
            }
            break;
        case BB_OTA_PHASE_FAIL:
        default:
            if (s_ops->restore) {
                s_ops->restore(s_ctx);
            }
            break;
    }
}
