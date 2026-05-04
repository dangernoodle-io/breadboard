// bb_led dispatch layer — platform-independent; kept in sync with platform/host/bb_led/bb_led.c.
#include "bb_led.h"
#include "bb_led_driver.h"
#include <stdlib.h>
#include <string.h>

struct bb_led {
    const bb_led_driver_t *drv;
    void *state;
};

static bool valid(bb_led_handle_t h)                    { return h && h->drv; }
static bool has_cap(bb_led_handle_t h, bb_led_caps_t c) { return (h->drv->caps & c) != 0; }
static bool idx_ok(bb_led_handle_t h, uint16_t idx)     { return idx < h->drv->count; }

bb_err_t bb_led_handle_create(const bb_led_driver_t *drv, void *state, bb_led_handle_t *out)
{
    if (!drv || !out) return BB_ERR_INVALID_ARG;
    bb_led_handle_t h = (bb_led_handle_t)calloc(1, sizeof(struct bb_led));
    if (!h) return BB_ERR_NO_SPACE;
    h->drv   = drv;
    h->state = state;
    *out = h;
    return BB_OK;
}

bb_led_caps_t bb_led_caps(bb_led_handle_t h)  { return valid(h) ? h->drv->caps  : BB_LED_CAP_NONE; }
uint16_t      bb_led_count(bb_led_handle_t h) { return valid(h) ? h->drv->count : 0; }

bb_err_t bb_led_set_on(bb_led_handle_t h, uint16_t idx, bool on)
{
    if (!valid(h))       return BB_ERR_INVALID_STATE;
    if (!idx_ok(h, idx)) return BB_ERR_INVALID_ARG;
    if (!has_cap(h, BB_LED_CAP_ONOFF)) return BB_ERR_UNSUPPORTED;
    return h->drv->set_on(h->state, idx, on);
}

bb_err_t bb_led_set_brightness(bb_led_handle_t h, uint16_t idx, uint8_t pct)
{
    if (!valid(h))                    return BB_ERR_INVALID_STATE;
    if (!idx_ok(h, idx) || pct > 100) return BB_ERR_INVALID_ARG;
    if (!has_cap(h, BB_LED_CAP_BRIGHTNESS)) return BB_ERR_UNSUPPORTED;
    return h->drv->set_brightness(h->state, idx, pct);
}

bb_err_t bb_led_set_color(bb_led_handle_t h, uint16_t idx, uint8_t r, uint8_t g, uint8_t b)
{
    if (!valid(h))       return BB_ERR_INVALID_STATE;
    if (!idx_ok(h, idx)) return BB_ERR_INVALID_ARG;
    if (!has_cap(h, BB_LED_CAP_RGB)) return BB_ERR_UNSUPPORTED;
    return h->drv->set_color(h->state, idx, r, g, b);
}

bb_err_t bb_led_fill_color(bb_led_handle_t h, uint8_t r, uint8_t g, uint8_t b)
{
    if (!valid(h)) return BB_ERR_INVALID_STATE;
    if (!has_cap(h, BB_LED_CAP_RGB)) return BB_ERR_UNSUPPORTED;
    for (uint16_t i = 0; i < h->drv->count; i++) {
        bb_err_t rc = h->drv->set_color(h->state, i, r, g, b);
        if (rc != BB_OK) return rc;
    }
    return BB_OK;
}

bb_err_t bb_led_flush(bb_led_handle_t h)
{
    if (!valid(h)) return BB_ERR_INVALID_STATE;
    if (!h->drv->flush) return BB_OK;
    return h->drv->flush(h->state);
}

bb_err_t bb_led_close(bb_led_handle_t h)
{
    if (!valid(h)) return BB_ERR_INVALID_STATE;
    bb_err_t rc = h->drv->close ? h->drv->close(h->state) : BB_OK;
    free(h);
    return rc;
}
