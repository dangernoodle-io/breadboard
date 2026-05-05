// bb_button dispatch layer — platform-independent; kept in sync with platform/espidf/bb_button/bb_button.c.
//
// When BB_BUTTON_MOCK_CLOCK is defined (test builds), bb_button_dispatch_raw
// accepts caller-supplied now_ms directly (no clock needed in the parent itself).
// The parent does not call now_ms(); the driver supplies the timestamp.
#include "bb_button.h"
#include "bb_button_driver.h"
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#endif

struct bb_button {
    const bb_button_driver_t *drv;
    void *state;
    bb_button_cb_t cb;
    void *user;
    bool last_debounced;
    uint32_t last_change_ms;
    uint16_t debounce_ms;
    bool closed;
    void *queue; // QueueHandle_t on IDF, else NULL
};

static bool valid(bb_button_handle_t h) { return h && h->drv && !h->closed; }

bb_err_t bb_button_handle_create(const bb_button_driver_t *drv, void *state, bb_button_handle_t *out)
{
    if (!drv || !out) return BB_ERR_INVALID_ARG;
    bb_button_handle_t h = (bb_button_handle_t)calloc(1, sizeof(struct bb_button));
    if (!h) return BB_ERR_NO_SPACE;
    h->drv         = drv;
    h->state       = state;
    h->debounce_ms = drv->debounce_ms ? drv->debounce_ms : 25;
#ifdef ESP_PLATFORM
    h->queue = xQueueCreate(8, sizeof(bb_button_event_t));
#else
    h->queue = NULL;
#endif
    *out = h;
    return BB_OK;
}

bb_err_t bb_button_dispatch_raw(bb_button_handle_t h, bool raw_pressed, uint32_t now_ms)
{
    if (!valid(h)) return BB_ERR_INVALID_STATE;

    // Debounce: ignore edges that arrive too soon after the last accepted edge.
    if ((now_ms - h->last_change_ms) < h->debounce_ms) return BB_OK;

    h->last_change_ms = now_ms;

    if (raw_pressed == h->last_debounced) return BB_OK;

    h->last_debounced = raw_pressed;

    bb_button_event_t ev = {
        .kind         = raw_pressed ? BB_BTN_PRESSED : BB_BTN_RELEASED,
        .timestamp_ms = now_ms,
    };

    if (h->cb) h->cb(&ev, h->user);

#ifdef ESP_PLATFORM
    if (h->queue) xQueueSend(h->queue, &ev, 0);
#endif

    return BB_OK;
}

bb_err_t bb_button_set_callback(bb_button_handle_t h, bb_button_cb_t cb, void *user)
{
    if (!valid(h)) return BB_ERR_INVALID_STATE;
    h->cb   = cb;
    h->user = user;
    return BB_OK;
}

bb_err_t bb_button_poll(bb_button_handle_t h)
{
    if (!valid(h)) return BB_ERR_INVALID_STATE;
    return h->drv->poll(h->state);
}

bool bb_button_is_pressed(bb_button_handle_t h)
{
    if (!valid(h)) return false;
    return h->last_debounced;
}

void *bb_button_get_queue(bb_button_handle_t h)
{
    if (!h) return NULL;
    return h->queue;
}

bb_err_t bb_button_close(bb_button_handle_t h)
{
    if (!h || !h->drv) return BB_ERR_INVALID_STATE;
    if (h->closed) return BB_OK;
    h->closed = true;
    bb_err_t rc = h->drv->close ? h->drv->close(h->state) : BB_OK;
#ifdef ESP_PLATFORM
    if (h->queue) { vQueueDelete(h->queue); h->queue = NULL; }
#endif
    free(h);
    return rc;
}
