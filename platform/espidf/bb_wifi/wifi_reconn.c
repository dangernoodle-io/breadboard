#include "wifi_reconn.h"
#include "wifi_reconn_policy.h"
#include "bb_nv.h"

#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include "bb_log.h"
#include "esp_timer.h"
#include "esp_system.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char *TAG = "wifi_reconn";

#define RECONN_QUEUE_LEN 8
#define RECONN_TASK_STACK 4096
#define RECONN_TASK_PRIO 6
#define RECONN_TASK_CORE 0

typedef enum {
    EVT_DISCONNECT = 1,
    EVT_GOT_IP     = 2,
} reconn_evt_type_t;

typedef struct {
    reconn_evt_type_t type;
    uint8_t reason;
} reconn_evt_t;

typedef enum {
    ST_IDLE,
    ST_BACKOFF,
    ST_CONNECTING,
} reconn_state_t;

// Task + queue handles
static TaskHandle_t  s_task  = NULL;
static QueueHandle_t s_queue = NULL;
static volatile bool s_active = false;

// Policy state. Manager task is single writer.
static wifi_reconn_state_t s_state = {0};

// Adapter to provide current time to policy module.
static int64_t reconn_now_us(void)
{
    return esp_timer_get_time();
}

static const wifi_reconn_adapter_t s_adapter = {
    .now_us = reconn_now_us,
};

static void handle_disconnect(uint8_t reason, reconn_state_t *state, uint32_t *backoff_ms)
{
    wifi_reconn_action_t action = wifi_reconn_policy_on_disconnect(
        &s_state, &s_adapter, reason, WIFI_REASON_HANDSHAKE_TIMEOUT, backoff_ms);

    switch (action) {
        case WIFI_RECONN_ACTION_REBOOT:
            bb_log_e(TAG, "persistent disconnect for >5min (reason=%u, handshake=%d, generic=%d), rebooting",
                     reason, s_state.handshake_fail_count, s_state.generic_fail_count);
            bb_nv_config_increment_boot_count();
            esp_restart();
            break;

        case WIFI_RECONN_ACTION_SCHEDULE_BACKOFF:
            *state = ST_BACKOFF;
            bb_log_w(TAG, "disconnect reason=%u, backoff=%ums (handshake=%d, generic=%d)",
                     reason, (unsigned)(*backoff_ms), s_state.handshake_fail_count,
                     s_state.generic_fail_count);
            break;

        case WIFI_RECONN_ACTION_RECONNECT_NOW:
            *state = ST_BACKOFF;
            bb_log_w(TAG, "disconnect reason=%u, immediate retry (handshake=%d, generic=%d)",
                     reason, s_state.handshake_fail_count, s_state.generic_fail_count);
            break;

        case WIFI_RECONN_ACTION_NONE:
        default:
            break;
    }
}

static void handle_got_ip(reconn_state_t *state, uint32_t *backoff_ms)
{
    bb_log_i(TAG, "got ip, reconnect manager idle");
    wifi_reconn_policy_on_got_ip(&s_state);
    *backoff_ms = 0;
    *state = ST_IDLE;
}

static void reconn_task(void *arg)
{
    (void)arg;
    reconn_state_t state = ST_IDLE;
    uint32_t backoff_ms = 0;

    bb_log_i(TAG, "reconnect manager started");

    for (;;) {
        TickType_t wait;
        switch (state) {
            case ST_BACKOFF:   wait = pdMS_TO_TICKS(backoff_ms); break;
            case ST_IDLE:
            case ST_CONNECTING:
            default:           wait = portMAX_DELAY;            break;
        }

        reconn_evt_t evt;
        BaseType_t got = xQueueReceive(s_queue, &evt, wait);
        if (got == pdTRUE) {
            switch (evt.type) {
                case EVT_DISCONNECT:
                    handle_disconnect(evt.reason, &state, &backoff_ms);
                    if (backoff_ms == 0) {
                        // Immediate retry
                        bb_log_i(TAG, "immediate reconnect attempt");
                        esp_wifi_connect();
                        state = ST_CONNECTING;
                    }
                    break;
                case EVT_GOT_IP:
                    handle_got_ip(&state, &backoff_ms);
                    break;
            }
        } else {
            // Backoff elapsed
            bb_log_i(TAG, "backoff elapsed, reconnecting");
            esp_wifi_connect();
            state = ST_CONNECTING;
        }
    }
}

void wifi_reconn_start(void)
{
    if (s_active) return;
    s_queue = xQueueCreate(RECONN_QUEUE_LEN, sizeof(reconn_evt_t));
    if (!s_queue) {
        bb_log_e(TAG, "failed to create reconn queue");
        return;
    }
    BaseType_t ok = xTaskCreatePinnedToCore(
        reconn_task, "wifi_reconn",
        RECONN_TASK_STACK, NULL,
        RECONN_TASK_PRIO, &s_task,
        RECONN_TASK_CORE);
    if (ok != pdPASS) {
        bb_log_e(TAG, "failed to create reconn task");
        vQueueDelete(s_queue);
        s_queue = NULL;
        return;
    }
    s_active = true;
}

bool wifi_reconn_is_active(void)
{
    return s_active;
}

void wifi_reconn_on_disconnect(uint8_t reason)
{
    if (!s_queue) return;
    reconn_evt_t evt = { .type = EVT_DISCONNECT, .reason = reason };
    if (xQueueSend(s_queue, &evt, 0) != pdTRUE) {
        bb_log_w(TAG, "queue full, dropping disconnect event");
    }
}

void wifi_reconn_on_got_ip(void)
{
    if (!s_queue) return;
    reconn_evt_t evt = { .type = EVT_GOT_IP, .reason = 0 };
    if (xQueueSend(s_queue, &evt, 0) != pdTRUE) {
        bb_log_w(TAG, "queue full, dropping got-ip event");
    }
}

void wifi_reconn_get_disconnect(uint8_t *reason, int64_t *age_us)
{
    if (reason) *reason = s_state.last_reason;
    if (age_us) {
        int64_t t = s_state.last_disconnect_us;
        *age_us = (t == 0) ? 0 : (esp_timer_get_time() - t);
    }
}

int wifi_reconn_get_retry_count(void)
{
    return s_state.retry_count;
}

void wifi_reconn_get_histogram(uint16_t *out, size_t len)
{
    if (!out) return;
    if (len > 256) len = 256;
    for (size_t i = 0; i < len; i++) {
        out[i] = s_state.reason_histogram[i];
    }
}
