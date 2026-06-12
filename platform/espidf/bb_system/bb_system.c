#include "bb_system.h"

#include "bb_log.h"

#include "esp_app_desc.h"
#include "esp_system.h"
#include "soc/soc_caps.h"

#if __has_include("bb_version_gen.h")
#include "bb_version_gen.h"
#endif

#if SOC_TEMP_SENSOR_SUPPORTED
#include "driver/temperature_sensor.h"
#include <pthread.h>
#endif

static const char *TAG = "bb_system";

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
#ifdef BB_FW_VERSION_STR
    return BB_FW_VERSION_STR;
#else
    const esp_app_desc_t *app = esp_app_get_description();
    return (app && app->version[0]) ? app->version : "0.0.0";
#endif
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
