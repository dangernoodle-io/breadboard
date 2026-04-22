#include "bb_system.h"

#include "bb_log.h"

#include "esp_system.h"

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

// Forward-declare bb_system_get_version from bb_http.h (defined in platform/espidf/system/system.c)
extern void bb_system_get_version(char *out, size_t out_size);

void bb_system_log_boot_info(void)
{
    const char *reason_str = bb_system_reset_reason_str(bb_system_get_reset_reason());
    char version[64];
    bb_system_get_version(version, sizeof(version));
    bb_log_i(TAG, "boot: reset=%s version=%s", reason_str, version);
}
