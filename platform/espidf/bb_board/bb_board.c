#include "bb_board.h"
#include "bb_meminfo.h"
#include "bb_str.h"

#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_private/esp_clk.h"

// FIRMWARE_BOARD must be supplied by the consumer as a C string literal,
// e.g. add_compile_definitions(FIRMWARE_BOARD="bitaxe-601"). Don't re-stringify
// it — downstream CMake typically injects the quotes already.
#ifndef FIRMWARE_BOARD
#define FIRMWARE_BOARD ""
#endif

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

// Intentionally separate from bb_system's BB_RESET_REASON_LIST X-macro: this is
// a raw esp_reset_reason_t mapping with narrower coverage (ext/int_wdt/sdio fall
// through to "unknown"). Unifying it would change those output values — a
// behavior change out of scope for B1-463's value-preserving pass. Follow-up.
static const char *reset_reason_str(esp_reset_reason_t r)
{
    switch (r) {
        case ESP_RST_POWERON:   return "power-on";
        case ESP_RST_SW:        return "software";
        case ESP_RST_PANIC:     return "panic";
        case ESP_RST_TASK_WDT:  return "task_wdt";
        case ESP_RST_WDT:       return "wdt";
        case ESP_RST_DEEPSLEEP: return "deep_sleep";
        case ESP_RST_BROWNOUT:  return "brownout";
        default:                return "unknown";
    }
}

bb_err_t bb_board_get_info(bb_board_info_t *out)
{
    if (!out) return BB_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));

    bb_strlcpy(out->board, FIRMWARE_BOARD, sizeof(out->board));

    const esp_app_desc_t *app = esp_app_get_description();
    if (app) {
        bb_strlcpy(out->project_name, app->project_name, sizeof(out->project_name));
        bb_strlcpy(out->version,      app->version,      sizeof(out->version));
        bb_strlcpy(out->idf_version,  app->idf_ver,      sizeof(out->idf_version));
        bb_strlcpy(out->build_date,   app->date,         sizeof(out->build_date));
        bb_strlcpy(out->build_time,   app->time,         sizeof(out->build_time));
    }

    esp_chip_info_t chip;
    esp_chip_info(&chip);
    bb_strlcpy(out->chip_model, chip_model_str(chip.model), sizeof(out->chip_model));
    out->cores = chip.cores;

    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        snprintf(out->mac, sizeof(out->mac), "%02x:%02x:%02x:%02x:%02x:%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }

    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);
    out->flash_size = flash_size;

    out->total_heap = (uint32_t)heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    out->free_heap  = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);

    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running) {
        out->app_size = running->size;
        esp_ota_img_states_t state;
        if (esp_ota_get_state_partition(running, &state) == ESP_OK) {
            out->ota_validated = (state != ESP_OTA_IMG_PENDING_VERIFY);
        } else {
            out->ota_validated = true;
        }
    } else {
        out->ota_validated = true;
    }

    bb_strlcpy(out->reset_reason, reset_reason_str(esp_reset_reason()),
               sizeof(out->reset_reason));

    return BB_OK;
}

uint32_t bb_board_get_free_heap(void)
{
    return (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
}

uint32_t bb_board_get_total_heap(void)
{
    return (uint32_t)heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
}

uint32_t bb_board_get_flash_size(void)
{
    uint32_t size = 0;
    esp_flash_get_size(NULL, &size);
    return size;
}

uint32_t bb_board_get_app_size(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    return running ? running->size : 0;
}

uint8_t bb_board_get_cores(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    return chip.cores;
}

static bb_err_t copy_str(char *out, size_t out_size, const char *src)
{
    if (!out || out_size == 0) return BB_ERR_INVALID_ARG;
    bb_strlcpy(out, src ? src : "", out_size);
    return BB_OK;
}

bb_err_t bb_board_get_chip_model(char *out, size_t out_size)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    return copy_str(out, out_size, chip_model_str(chip.model));
}

bb_err_t bb_board_get_mac(char *out, size_t out_size)
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

bb_err_t bb_board_get_idf_version(char *out, size_t out_size)
{
    const esp_app_desc_t *app = esp_app_get_description();
    return copy_str(out, out_size, app ? app->idf_ver : "");
}

bb_err_t bb_board_get_reset_reason(char *out, size_t out_size)
{
    return copy_str(out, out_size, reset_reason_str(esp_reset_reason()));
}

// bb_board_heap_* / bb_board_psram_* / bb_board_rtc_* / bb_board_dram_static_bytes
// delegate to bb_meminfo — the canonical heap_caps reader SSOT (KB #698/#699/
// #693). Each call re-derives a full snapshot; these are cold-path diagnostic
// accessors, not hot-path, so the extra heap_caps_* calls per single-field
// read are an acceptable tradeoff for a single heap_caps call site.
size_t bb_board_heap_free_total(void)
{
    bb_meminfo_snapshot_t m;
    bb_meminfo_get(&m);
    return m.default_region.free;
}

size_t bb_board_heap_free_internal(void)
{
    bb_meminfo_snapshot_t m;
    bb_meminfo_get(&m);
    return m.internal.free;
}

size_t bb_board_heap_minimum_ever(void)
{
    bb_meminfo_snapshot_t m;
    bb_meminfo_get(&m);
    return m.default_region.min_ever_free;
}

size_t bb_board_heap_internal_minimum_ever(void)
{
    bb_meminfo_snapshot_t m;
    bb_meminfo_get(&m);
    return m.internal.min_ever_free;
}

size_t bb_board_heap_largest_free_block(void)
{
    bb_meminfo_snapshot_t m;
    bb_meminfo_get(&m);
    return m.default_region.largest_free_block;
}

size_t bb_board_heap_internal_largest_free_block(void)
{
    bb_meminfo_snapshot_t m;
    bb_meminfo_get(&m);
    return m.internal.largest_free_block;
}

uint32_t bb_board_chip_revision(void)
{
    esp_chip_info_t info;
    esp_chip_info(&info);
    return info.revision;
}

uint32_t bb_board_cpu_freq_mhz(void)
{
    return (uint32_t)(esp_clk_cpu_freq() / 1000000);
}

size_t bb_board_heap_internal_free(void)
{
    bb_meminfo_snapshot_t m;
    bb_meminfo_get(&m);
    return m.internal.free;
}

size_t bb_board_heap_internal_total(void)
{
    bb_meminfo_snapshot_t m;
    bb_meminfo_get(&m);
    return m.internal.total;
}

size_t bb_board_psram_free(void)
{
    bb_meminfo_snapshot_t m;
    bb_meminfo_get(&m);
    return m.spiram.free;
}

size_t bb_board_psram_total(void)
{
    bb_meminfo_snapshot_t m;
    bb_meminfo_get(&m);
    return m.spiram.total;
}

// RTC slow memory + internal DRAM static accounting now live in bb_meminfo
// (platform/espidf/bb_meminfo/bb_meminfo.c carries the full linker-symbol
// derivation this used to duplicate). Thin delegators kept here for API
// stability of bb_board's existing callers.
size_t bb_board_rtc_used(void)
{
    bb_meminfo_snapshot_t m;
    bb_meminfo_get(&m);
    return m.rtc_used;
}

size_t bb_board_rtc_total(void)
{
    bb_meminfo_snapshot_t m;
    bb_meminfo_get(&m);
    return m.rtc_total;
}

size_t bb_board_dram_static_bytes(void)
{
    bb_meminfo_snapshot_t m;
    bb_meminfo_get(&m);
    return m.dram_static_bytes;
}
