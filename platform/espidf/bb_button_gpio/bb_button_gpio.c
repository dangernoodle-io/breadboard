// bb_button_gpio — ESP-IDF GPIO driver.
// Uses gpio_install_isr_service + gpio_isr_handler_add. An ISR pushes a
// marker into a per-instance queue; a per-instance service task drains the
// queue, reads the actual pin level, and calls bb_button_dispatch_raw().
// poll() is a no-op — dispatch happens on the service task.
#include "bb_button_gpio.h"
#include "bb_button_driver.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_timer.h"
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

// Guard: gpio_install_isr_service is idempotent but logs a warning on repeat.
static bool s_isr_service_installed = false;

typedef struct {
    int gpio;
    bool active_low;
    bb_button_handle_t handle;
    QueueHandle_t intr_q;     // ISR → task
    TaskHandle_t task;
    bb_button_driver_t drv;   // per-instance vtable
} state_t;

// ISR context — push a marker (any value) into the intr queue.
static void IRAM_ATTR isr_handler(void *arg)
{
    state_t *s = (state_t *)arg;
    uint8_t marker = 1;
    BaseType_t woken = pdFALSE;
    xQueueSendFromISR(s->intr_q, &marker, &woken);
    if (woken) portYIELD_FROM_ISR();
}

// Service task — drains ISR markers, reads actual pin level, dispatches.
static void service_task(void *arg)
{
    state_t *s = (state_t *)arg;
    uint8_t marker;
    for (;;) {
        if (xQueueReceive(s->intr_q, &marker, portMAX_DELAY) == pdTRUE) {
            int level = gpio_get_level(s->gpio);
            bool raw = s->active_low ? (level == 0) : (level == 1);
            uint32_t now = (uint32_t)(esp_timer_get_time() / 1000LL);
            bb_button_dispatch_raw(s->handle, raw, now);
        }
    }
}

static bool op_is_pressed(void *st)
{
    return bb_button_is_pressed(((state_t *)st)->handle);
}

static bb_err_t op_poll(void *st)
{
    // Full-ISR backend — poll is a no-op.
    (void)st;
    return BB_OK;
}

static bb_err_t op_close(void *st)
{
    state_t *s = (state_t *)st;
    gpio_isr_handler_remove(s->gpio);
    if (s->task)   { vTaskDelete(s->task);   s->task   = NULL; }
    if (s->intr_q) { vQueueDelete(s->intr_q); s->intr_q = NULL; }
    gpio_reset_pin(s->gpio);
    free(s);
    return BB_OK;
}

bb_err_t bb_button_gpio_open(const bb_button_gpio_cfg_t *cfg, bb_button_handle_t *out)
{
    if (!cfg || !out) return BB_ERR_INVALID_ARG;
    if (cfg->gpio < 0) return BB_ERR_INVALID_ARG;

    if (!s_isr_service_installed) {
        gpio_install_isr_service(0);
        s_isr_service_installed = true;
    }

    state_t *s = (state_t *)calloc(1, sizeof(state_t));
    if (!s) return BB_ERR_NO_SPACE;
    s->gpio       = cfg->gpio;
    s->active_low = cfg->active_low;

    s->drv.is_pressed  = op_is_pressed;
    s->drv.poll        = op_poll;
    s->drv.close       = op_close;
    s->drv.debounce_ms = cfg->debounce_ms ? cfg->debounce_ms : 25;

    s->intr_q = xQueueCreate(4, sizeof(uint8_t));
    if (!s->intr_q) { free(s); return BB_ERR_NO_SPACE; }

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << cfg->gpio,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = cfg->active_low ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = cfg->active_low ? GPIO_PULLDOWN_DISABLE : GPIO_PULLDOWN_ENABLE,
        .intr_type    = GPIO_INTR_ANYEDGE,
    };
    if (gpio_config(&io) != ESP_OK) {
        vQueueDelete(s->intr_q);
        free(s);
        return BB_ERR_INVALID_STATE;
    }

    bb_err_t rc = bb_button_handle_create(&s->drv, s, out);
    if (rc != BB_OK) {
        vQueueDelete(s->intr_q);
        gpio_reset_pin(cfg->gpio);
        free(s);
        return rc;
    }
    s->handle = *out;

    if (xTaskCreate(service_task, "bb_btn_gpio", 2048, s, tskIDLE_PRIORITY + 5, &s->task) != pdPASS) {
        bb_button_close(*out); // closes handle, calls op_close via drv
        *out = NULL;
        return BB_ERR_NO_SPACE;
    }

    gpio_isr_handler_add(cfg->gpio, isr_handler, s);

    return BB_OK;
}
