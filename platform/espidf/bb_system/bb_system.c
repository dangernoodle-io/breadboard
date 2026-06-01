#include "bb_system.h"

#include "bb_log.h"

#include "esp_app_desc.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "soc/soc_caps.h"

#if SOC_TEMP_SENSOR_SUPPORTED
#include "driver/temperature_sensor.h"
#include <pthread.h>
#endif

static const char *TAG = "bb_system";

void bb_system_wdt_set_timeout(uint32_t timeout_s)
{
    esp_task_wdt_config_t cfg = {
        .timeout_ms = timeout_s * 1000U,
        .idle_core_mask =
#if defined(CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0) && CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0
            (1U << 0) |
#endif
#if defined(CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1) && CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1
            (1U << 1) |
#endif
            0U,
        .trigger_panic =
#if defined(CONFIG_ESP_TASK_WDT_PANIC) && CONFIG_ESP_TASK_WDT_PANIC
            true,
#else
            false,
#endif
    };
    esp_err_t err = esp_task_wdt_reconfigure(&cfg);
    if (err != ESP_OK) {
        bb_log_w(TAG, "esp_task_wdt_reconfigure(%ums): %s",
                 (unsigned)cfg.timeout_ms, esp_err_to_name(err));
    }
}

bb_reset_reason_t bb_system_get_reset_reason(void)
{
    esp_reset_reason_t esp_reason = esp_reset_reason();
    switch (esp_reason) {
        case ESP_RST_UNKNOWN:
            return BB_RESET_REASON_UNKNOWN;
        case ESP_RST_POWERON:
            return BB_RESET_REASON_POWERON;
        case ESP_RST_EXT:
            return BB_RESET_REASON_EXT;
        case ESP_RST_SW:
            return BB_RESET_REASON_SW;
        case ESP_RST_PANIC:
            return BB_RESET_REASON_PANIC;
        case ESP_RST_INT_WDT:
            return BB_RESET_REASON_INT_WDT;
        case ESP_RST_TASK_WDT:
            return BB_RESET_REASON_TASK_WDT;
        case ESP_RST_WDT:
            return BB_RESET_REASON_WDT;
        case ESP_RST_DEEPSLEEP:
            return BB_RESET_REASON_DEEPSLEEP;
        case ESP_RST_BROWNOUT:
            return BB_RESET_REASON_BROWNOUT;
        case ESP_RST_SDIO:
            return BB_RESET_REASON_SDIO;
        // ESP-IDF may have additional reset reasons (USB, JTAG, etc.) that map to UNKNOWN
        case ESP_RST_USB:
        case ESP_RST_JTAG:
        case ESP_RST_EFUSE:
        case ESP_RST_PWR_GLITCH:
        case ESP_RST_CPU_LOCKUP:
        default:
            return BB_RESET_REASON_UNKNOWN;
    }
}

const char *bb_system_reset_reason_str(bb_reset_reason_t r)
{
    switch (r) {
        case BB_RESET_REASON_POWERON:
            return "power-on";
        case BB_RESET_REASON_EXT:
            return "ext";
        case BB_RESET_REASON_SW:
            return "software";
        case BB_RESET_REASON_PANIC:
            return "panic";
        case BB_RESET_REASON_INT_WDT:
            return "int_wdt";
        case BB_RESET_REASON_TASK_WDT:
            return "task_wdt";
        case BB_RESET_REASON_WDT:
            return "wdt";
        case BB_RESET_REASON_DEEPSLEEP:
            return "deep_sleep";
        case BB_RESET_REASON_BROWNOUT:
            return "brownout";
        case BB_RESET_REASON_SDIO:
            return "sdio";
        case BB_RESET_REASON_UNKNOWN:
        default:
            return "unknown";
    }
}

bool bb_system_is_abnormal_reset(void)
{
    esp_reset_reason_t reason = esp_reset_reason();
    // Match TaipanMiner's classification: only TASK_WDT, WDT, PANIC
    return reason == ESP_RST_TASK_WDT || reason == ESP_RST_WDT || reason == ESP_RST_PANIC;
}

const char *bb_system_get_version(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    return (app && app->version[0]) ? app->version : "0.0.0";
}

void bb_system_log_boot_info(void)
{
    const char *reason_str = bb_system_reset_reason_str(bb_system_get_reset_reason());
    bb_log_i(TAG, "boot: reset=%s version=%s", reason_str, bb_system_get_version());
}

const char *bb_system_get_project_name(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    return (app && app->project_name[0]) ? app->project_name : "unknown";
}

const char *bb_system_get_build_date(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    return (app && app->date[0]) ? app->date : "unknown";
}

const char *bb_system_get_build_time(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    return (app && app->time[0]) ? app->time : "unknown";
}

const char *bb_system_get_idf_version(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    return (app && app->idf_ver[0]) ? app->idf_ver : "0.0.0";
}

void bb_system_restart(void)
{
    esp_restart();
}

bb_err_t bb_system_read_temp_celsius(float *out)
{
    if (!out) return BB_ERR_INVALID_ARG;
#if SOC_TEMP_SENSOR_SUPPORTED
    static temperature_sensor_handle_t s_handle = NULL;
    static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&s_lock);
    if (s_handle == NULL) {
        temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
        esp_err_t err = temperature_sensor_install(&cfg, &s_handle);
        if (err == ESP_OK) err = temperature_sensor_enable(s_handle);
        if (err != ESP_OK) {
            if (s_handle) { temperature_sensor_uninstall(s_handle); s_handle = NULL; }
            pthread_mutex_unlock(&s_lock);
            return err;
        }
    }
    pthread_mutex_unlock(&s_lock);
    float c = 0.0f;
    esp_err_t err = temperature_sensor_get_celsius(s_handle, &c);
    if (err != ESP_OK) return err;
    *out = c;
    return BB_OK;
#else
    (void)out;
    return BB_ERR_UNSUPPORTED;
#endif
}
