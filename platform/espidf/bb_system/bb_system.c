#include "bb_system.h"

#include "bb_log.h"
#include "bb_config.h"
#include "bb_nv_namespaces.h"
#include "bb_nv_keys.h"
#include "bb_ntp.h"
#include "bb_clock.h"

#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_private/esp_clk.h"
#include "soc/soc_caps.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

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
#define X(v, s) case v: return s;
        BB_RESET_REASON_LIST(X)
#undef X
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

// ---------------------------------------------------------------------------
// HW-identity accessors (relocated from bb_board, B1-977 dissolution).
// ---------------------------------------------------------------------------

static const char *chip_model_str(esp_chip_model_t m)
{
    switch (m) {
        case CHIP_ESP32:    return "ESP32";
        case CHIP_ESP32S2:  return "ESP32-S2";
        case CHIP_ESP32S3:  return "ESP32-S3";
        case CHIP_ESP32C3:  return "ESP32-C3";
        case CHIP_ESP32C6:  return "ESP32-C6";
        case CHIP_ESP32H2:  return "ESP32-H2";
        case CHIP_ESP32P4:  return "ESP32-P4";
        default:            return "unknown";
    }
}

const char *bb_system_get_chip_model(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    return chip_model_str(chip.model);
}

uint8_t bb_system_get_cores(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    return chip.cores;
}

bb_err_t bb_system_get_mac(char *out, size_t out_size)
{
    if (!out || out_size == 0) return BB_ERR_INVALID_ARG;
    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
        out[0] = '\0';
        return BB_OK;
    }
    snprintf(out, out_size, "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return BB_OK;
}

uint32_t bb_system_get_flash_size(void)
{
    uint32_t size = 0;
    esp_flash_get_size(NULL, &size);
    return size;
}

uint32_t bb_system_get_app_size(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    return running ? running->size : 0;
}

uint32_t bb_system_chip_revision(void)
{
    esp_chip_info_t info;
    esp_chip_info(&info);
    return info.revision;
}

uint32_t bb_system_cpu_freq_mhz(void)
{
    return (uint32_t)(esp_clk_cpu_freq() / 1000000);
}

void bb_system_restart_reason(bb_reset_source_t src, const char *detail)
{
    bb_system_restart_reason_at(src, detail, 0);
}

void bb_system_restart_reason_at(bb_reset_source_t src, const char *detail, uint32_t caller_epoch_s)
{
    uint32_t uptime_s = (uint32_t)(bb_clock_now_ms64() / 1000ULL);

    bool ntp_synced = bb_ntp_is_synced();
    uint32_t device_epoch_s = ntp_synced ? (uint32_t)time(NULL) : 0U;
    uint32_t epoch_s = bb_reboot_pick_epoch(ntp_synced, device_epoch_s, caller_epoch_s, BB_REBOOT_EPOCH_FLOOR_S);

    bb_err_t err = bb_system_reboot_record_save(src, detail, epoch_s, uptime_s);
    if (err == BB_ERR_INVALID_ARG) {
        bb_log_w(TAG, "restart_reason: record encode failed, rebooting without reason");
    } else if (err != BB_OK) {
        bb_log_w(TAG, "restart_reason: NVS persist failed: %d", (int)err);
    }

    bb_log_i(TAG, "restart_reason: src=%s", bb_reset_source_str(src));
    esp_restart();
}

// Boot-health counter (B1-753) — co-located with the reboot-reason record
// under the same NVS namespace, round-tripped through bb_config (typed layer
// over bb_storage) rather than bb_nv's generic KV forwarder (B1-756, bb_nv
// dissolution epic B1-708) — bb_config's U8 encoding resolves to the SAME
// nvs_get_u8/nvs_set_u8 calls bb_nv_get_u8/set_u8 made (both are thin
// forwarders to bb_storage_nvs, see bb_storage_nvs.h), so the
// namespace/key/U8-typed on-flash format below is byte-compatible with what
// this counter previously read/wrote via bb_nv.
//
// This counter moved here from the old bb_cfg NVS namespace (bb_nv config
// API) to bb_reboot (BB_REBOOT_NVS_NS/BB_REBOOT_KEY_BOOT_CNT). An OTA from a
// pre-migration build therefore reads back 0 here, not the old bb_cfg
// value — the migration is non-preserving. This is safe: the counter is
// zeroed on every successful WiFi connect, and a device must have connected
// recently to fetch the OTA in the first place, so the reset only discards
// partial fail-progress on an already-healthy device. It never triggers a
// premature rollback (starts at 0, same as a fresh device) nor suppresses a
// needed one (a genuinely failing device keeps incrementing post-OTA). The
// stranded bb_cfg/boot_cnt byte is left in place, retired wholesale when
// bb_nv is deleted.
static const bb_config_field_t s_boot_count_field = {
    .id          = "system.boot_count",
    .type        = BB_CONFIG_U8,
    .addr        = { .backend = "nvs", .ns_or_dir = BB_REBOOT_NVS_NS, .key = BB_REBOOT_KEY_BOOT_CNT },
    .def         = { .u8 = 0 },
    .has_default = true,
};

bb_err_t bb_system_boot_count_increment(void)
{
    uint8_t val = 0;
    bb_config_get_u8(&s_boot_count_field, &val);
    if (val < UINT8_MAX) val++;
    return bb_config_set_u8(&s_boot_count_field, val);
}

bb_err_t bb_system_boot_count_reset(void)
{
    return bb_config_set_u8(&s_boot_count_field, 0);
}

uint8_t bb_system_boot_count_get(void)
{
    uint8_t val = 0;
    bb_config_get_u8(&s_boot_count_field, &val);
    return val;
}

// Reboot budget (B1-863) — real epoch resolution. See bb_system_reboot_
// budget.c's file header for why this split is per-platform, not shared.
bool bb_system_reboot_budget_allows(bb_reboot_cause_t cause)
{
    return bb_system_reboot_budget_allows_at(cause, bb_ntp_is_synced(), (uint32_t)time(NULL));
}

void bb_system_reboot_budget_record(bb_reboot_cause_t cause)
{
    bb_system_reboot_budget_record_at(cause, bb_ntp_is_synced(), (uint32_t)time(NULL));
}

// WiFi safeguard-reboot facade (B1-790 slice) -- real synced/epoch
// resolution. See bb_system_safeguard_reboot.c for the orchestration this
// delegates to.
bool bb_system_safeguard_reboot_allowed(bb_reboot_cause_t cause)
{
    return bb_system_safeguard_reboot_allowed_at(cause, bb_ntp_is_synced(), (uint32_t)time(NULL));
}

void bb_system_safeguard_reboot(bb_reboot_cause_t cause, bool ota_validated, const char *detail)
{
    bb_system_safeguard_reboot_account(cause, ota_validated, bb_ntp_is_synced());
    bb_system_restart_reason(bb_system_safeguard_reboot_src_for_cause(cause), detail);
}

// Number of hex chars in the app SHA256 prefix.
// Bridge pattern (see CLAUDE.md "Kconfig knobs must bridge CONFIG_").
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#ifdef CONFIG_APP_RETRIEVE_LEN_ELF_SHA
#define BB_APP_SHA_HEX_LEN CONFIG_APP_RETRIEVE_LEN_ELF_SHA
#endif
#endif
#ifndef BB_APP_SHA_HEX_LEN
#define BB_APP_SHA_HEX_LEN 9
#endif

bb_err_t bb_system_get_app_sha256(char *out, size_t out_size)
{
    if (!out || out_size == 0) return BB_ERR_INVALID_ARG;
    // Need BB_APP_SHA_HEX_LEN hex chars + NUL
    if (out_size <= BB_APP_SHA_HEX_LEN) return BB_ERR_NO_SPACE;
    const uint8_t *sha = esp_app_get_description()->app_elf_sha256;
    // Each byte produces 2 hex chars; generate up to BB_APP_SHA_HEX_LEN chars.
    size_t written = 0;
    for (size_t i = 0; written < BB_APP_SHA_HEX_LEN; i++) {
        char pair[3];
        snprintf(pair, sizeof(pair), "%02x", sha[i]);
        // pair[0] and pair[1] are the two hex chars for sha[i]
        if (written < BB_APP_SHA_HEX_LEN) {
            out[written++] = pair[0];
        }
        if (written < BB_APP_SHA_HEX_LEN) {
            out[written++] = pair[1];
        }
    }
    out[written] = '\0';
    return BB_OK;
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
