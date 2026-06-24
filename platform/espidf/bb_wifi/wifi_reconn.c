#include "wifi_reconn.h"
#include "wifi_reconn_policy.h"
#include "bb_nv.h"
#include "bb_ota_validator.h"
#include "bb_timer.h"

#include <stdio.h>

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
// 6144 (not 4096): do_safeguard_reboot() opens NVS + writes the boot-fail count
// (nvs_open/set/commit), which can consume ~1.5-2 KB of stack on top of the
// reconn task's normal usage — 4096 left too little headroom (B1-330).
#define RECONN_TASK_STACK 6144
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

// Set before issuing a self-induced esp_wifi_disconnect() in the connect-timeout
// path so the resulting WIFI_EVENT_STA_DISCONNECTED → EVT_DISCONNECT is
// recognized as self-induced and absorbed (no fail-count bump, no second
// esp_wifi_connect() call).
static volatile bool s_self_disconnect = false;

// Policy state. Manager task is single writer.
static wifi_reconn_state_t s_state = {0};

// Adapter to provide current time to policy module.
static int64_t reconn_now_us(void)
{
    return (int64_t)bb_timer_now_us();
}

static const wifi_reconn_adapter_t s_adapter = {
    .now_us = reconn_now_us,
};

static void do_safeguard_reboot(const char *ctx)
{
    if (bb_ota_is_validated()) {
        bb_log_w(TAG, "%s (handshake=%d, generic=%d) on validated firmware: safeguard reboot, boot_count not incremented",
                 ctx, s_state.handshake_fail_count, s_state.generic_fail_count);
        esp_restart();
    } else {
        bb_log_e(TAG, "%s (handshake=%d, generic=%d) for >5min, rebooting",
                 ctx, s_state.handshake_fail_count, s_state.generic_fail_count);
        bb_nv_config_increment_boot_count();
        esp_restart();
    }
}

static void handle_disconnect(uint8_t reason, reconn_state_t *state, uint32_t *backoff_ms)
{
    wifi_reconn_action_t action = wifi_reconn_policy_on_disconnect(
        &s_state, &s_adapter, reason, WIFI_REASON_HANDSHAKE_TIMEOUT, backoff_ms);

    switch (action) {
        case WIFI_RECONN_ACTION_REBOOT: {
            char ctx[64];
            snprintf(ctx, sizeof(ctx), "persistent disconnect (reason=%u)", (unsigned)reason);
            do_safeguard_reboot(ctx);
            break;
        }

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
            case ST_BACKOFF:    wait = pdMS_TO_TICKS(backoff_ms);                        break;
            case ST_CONNECTING: wait = pdMS_TO_TICKS(WIFI_RECONN_CONNECTING_TIMEOUT_MS); break;
            case ST_IDLE:
            default:            wait = portMAX_DELAY;                                    break;
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
        } else if (state == ST_CONNECTING) {
            // Connect watchdog fired: no GOT_IP or DISCONNECT within timeout.
            uint32_t dummy_backoff = 0;
            wifi_reconn_action_t action = wifi_reconn_policy_on_connect_timeout(
                &s_state, &s_adapter, &dummy_backoff);
            if (action == WIFI_RECONN_ACTION_REBOOT) {
                do_safeguard_reboot("connect stall");
            } else {
                bb_log_w(TAG, "connect stalled %dms, re-attempting (generic=%d)",
                         WIFI_RECONN_CONNECTING_TIMEOUT_MS, s_state.generic_fail_count);
                s_self_disconnect = true;  // absorb the DISCONNECTED event this triggers
                esp_wifi_disconnect();
                esp_wifi_connect();
                // stay ST_CONNECTING
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
    // Absorb disconnects that we triggered ourselves (connect-timeout re-attempt).
    // esp_wifi_disconnect() emits WIFI_EVENT_STA_DISCONNECTED synchronously before
    // returning, so by the time this function is called s_self_disconnect is already
    // set. Clearing it here prevents a double generic_fail_count bump and a
    // spurious second esp_wifi_connect() from handle_disconnect().
    if (s_self_disconnect) {
        s_self_disconnect = false;
        bb_log_d(TAG, "absorbing self-induced disconnect (connect-timeout re-attempt)");
        return;
    }
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
        *age_us = (t == 0) ? 0 : ((int64_t)bb_timer_now_us() - t);
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
