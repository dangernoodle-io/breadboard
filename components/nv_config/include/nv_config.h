#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef ESP_PLATFORM
#include "esp_err.h"
typedef esp_err_t bsp_err_t;
#else
#define ESP_OK 0
typedef int bsp_err_t;
#endif

bsp_err_t bsp_nv_config_init(void);

const char *bsp_nv_config_wifi_ssid(void);
const char *bsp_nv_config_wifi_pass(void);
bool bsp_nv_config_display_enabled(void);

#ifdef ESP_PLATFORM
bool bsp_nv_config_is_provisioned(void);
bsp_err_t bsp_nv_config_set_provisioned(void);
bsp_err_t bsp_nv_config_clear_provisioned(void);
bsp_err_t bsp_nv_config_clear_wifi(void);

#define BSP_NV_CONFIG_BOOT_FAIL_THRESHOLD 3
uint8_t   bsp_nv_config_boot_count(void);
bsp_err_t bsp_nv_config_increment_boot_count(void);
bsp_err_t bsp_nv_config_reset_boot_count(void);

bool      bsp_nv_config_ota_skip_check(void);
bsp_err_t bsp_nv_config_set_ota_skip_check(bool skip);

bsp_err_t bsp_nv_config_set_wifi(const char *ssid, const char *pass);
bsp_err_t bsp_nv_config_set_display_enabled(bool en);
#else
static inline bool bsp_nv_config_is_provisioned(void) { return false; }
#endif
