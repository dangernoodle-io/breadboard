#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef ESP_PLATFORM
#include "esp_err.h"
typedef esp_err_t bb_err_t;
#else
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_NVS_NOT_FOUND 2
#define ESP_ERR_NVS_NOT_INITIALIZED 3
typedef int bb_err_t;
#endif

bb_err_t bb_nv_config_init(void);

const char *bb_nv_config_wifi_ssid(void);
const char *bb_nv_config_wifi_pass(void);
bool bb_nv_config_display_enabled(void);

#ifdef ESP_PLATFORM
bool bb_nv_config_is_provisioned(void);
bb_err_t bb_nv_config_set_provisioned(void);
bb_err_t bb_nv_config_clear_provisioned(void);
bb_err_t bb_nv_config_clear_wifi(void);

#define BB_NV_CONFIG_BOOT_FAIL_THRESHOLD 3
uint8_t   bb_nv_config_boot_count(void);
bb_err_t bb_nv_config_increment_boot_count(void);
bb_err_t bb_nv_config_reset_boot_count(void);

bool      bb_nv_config_ota_skip_check(void);
bb_err_t bb_nv_config_set_ota_skip_check(bool skip);

bb_err_t bb_nv_config_set_wifi(const char *ssid, const char *pass);
bb_err_t bb_nv_config_set_display_enabled(bool en);
#else
static inline bool bb_nv_config_is_provisioned(void) { return false; }
#endif

bb_err_t bb_nv_set_u8 (const char *ns, const char *key, uint8_t value);
bb_err_t bb_nv_set_u32(const char *ns, const char *key, uint32_t value);
bb_err_t bb_nv_set_str(const char *ns, const char *key, const char *value);
bb_err_t bb_nv_get_u8 (const char *ns, const char *key, uint8_t  *out, uint8_t  fallback);
bb_err_t bb_nv_get_u32(const char *ns, const char *key, uint32_t *out, uint32_t fallback);
bb_err_t bb_nv_get_str(const char *ns, const char *key, char *buf, size_t len, const char *fallback);
bb_err_t bb_nv_erase  (const char *ns, const char *key);
