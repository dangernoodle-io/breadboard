#include "nv_config.h"
#include <string.h>

#ifndef BB_NV_CONFIG_NAMESPACE
#define BB_NV_CONFIG_NAMESPACE "bb_cfg"
#endif

static struct {
    char wifi_ssid[32];
    char wifi_pass[64];
    uint8_t display_en;
} s_config;

// Helper to load a string from NVS with fallback (ESP only)
#ifdef ESP_PLATFORM
#include "nvs_flash.h"
#include "nvs.h"
#include "log_stream.h"
static const char *TAG = "nv_config";

static void load_str(nvs_handle_t handle, const char *key, char *buf, size_t buf_size, const char *fallback)
{
    size_t len = buf_size;
    if (nvs_get_str(handle, key, buf, &len) != ESP_OK) {
        strlcpy(buf, fallback, buf_size);
    }
}
#endif

bb_err_t bb_nv_config_init(void)
{
#ifdef ESP_PLATFORM
    nvs_handle_t handle;
    bb_err_t err = nvs_open(BB_NV_CONFIG_NAMESPACE, NVS_READONLY, &handle);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        bb_log_i(TAG, "no config in NVS");
        memset(&s_config, 0, sizeof(s_config));
        s_config.display_en = 1;  // default: display on
        return ESP_OK;
    }

    if (err != ESP_OK) {
        return err;
    }

    load_str(handle, "wifi_ssid", s_config.wifi_ssid, sizeof(s_config.wifi_ssid), "");
    load_str(handle, "wifi_pass", s_config.wifi_pass, sizeof(s_config.wifi_pass), "");

    if (nvs_get_u8(handle, "display_en", &s_config.display_en) != ESP_OK) {
        s_config.display_en = 1;  // default: display on
    }

    nvs_close(handle);

    bb_log_i(TAG, "config loaded");
#else
    // Native build: no NVS, all fields empty/zero
    memset(&s_config, 0, sizeof(s_config));
    s_config.display_en = 1;
#endif
    return ESP_OK;
}

#ifdef ESP_PLATFORM
bool bb_nv_config_is_provisioned(void)
{
    nvs_handle_t handle;
    bb_err_t err = nvs_open(BB_NV_CONFIG_NAMESPACE, NVS_READONLY, &handle);

    if (err != ESP_OK) {
        return false;
    }

    uint8_t value = 0;
    err = nvs_get_u8(handle, "provisioned", &value);
    nvs_close(handle);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return false;
    }

    return value == 1;
}

bb_err_t bb_nv_config_set_provisioned(void)
{
    nvs_handle_t handle;
    bb_err_t err = nvs_open(BB_NV_CONFIG_NAMESPACE, NVS_READWRITE, &handle);

    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_u8(handle, "provisioned", 1);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    return err;
}

bb_err_t bb_nv_config_set_wifi(const char *ssid, const char *pass)
{
    nvs_handle_t handle;
    bb_err_t err = nvs_open(BB_NV_CONFIG_NAMESPACE, NVS_READWRITE, &handle);

    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(handle, "wifi_ssid", ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, "wifi_pass", pass);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err == ESP_OK) {
        strncpy(s_config.wifi_ssid, ssid, sizeof(s_config.wifi_ssid) - 1);
        s_config.wifi_ssid[sizeof(s_config.wifi_ssid) - 1] = '\0';
        strncpy(s_config.wifi_pass, pass, sizeof(s_config.wifi_pass) - 1);
        s_config.wifi_pass[sizeof(s_config.wifi_pass) - 1] = '\0';
    }

    return err;
}

bb_err_t bb_nv_config_set_display_enabled(bool en)
{
    nvs_handle_t handle;
    bb_err_t err = nvs_open(BB_NV_CONFIG_NAMESPACE, NVS_READWRITE, &handle);

    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_u8(handle, "display_en", en ? 1 : 0);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err == ESP_OK) {
        s_config.display_en = en ? 1 : 0;
    }

    return err;
}

bb_err_t bb_nv_config_clear_provisioned(void)
{
    nvs_handle_t handle;
    bb_err_t err = nvs_open(BB_NV_CONFIG_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_erase_key(handle, "provisioned");
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

bb_err_t bb_nv_config_clear_wifi(void)
{
    nvs_handle_t handle;
    bb_err_t err = nvs_open(BB_NV_CONFIG_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_erase_key(handle, "wifi_ssid");
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) {
        err = nvs_erase_key(handle, "wifi_pass");
        if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    }
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err == ESP_OK) {
        s_config.wifi_ssid[0] = '\0';
        s_config.wifi_pass[0] = '\0';
    }
    return err;
}

uint8_t bb_nv_config_boot_count(void)
{
    nvs_handle_t handle;
    if (nvs_open(BB_NV_CONFIG_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return 0;
    uint8_t val = 0;
    nvs_get_u8(handle, "boot_cnt", &val);
    nvs_close(handle);
    return val;
}

bb_err_t bb_nv_config_increment_boot_count(void)
{
    nvs_handle_t handle;
    bb_err_t err = nvs_open(BB_NV_CONFIG_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    uint8_t val = 0;
    nvs_get_u8(handle, "boot_cnt", &val);
    if (val < UINT8_MAX) val++;
    err = nvs_set_u8(handle, "boot_cnt", val);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

bb_err_t bb_nv_config_reset_boot_count(void)
{
    nvs_handle_t handle;
    bb_err_t err = nvs_open(BB_NV_CONFIG_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(handle, "boot_cnt", 0);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

bool bb_nv_config_ota_skip_check(void)
{
    nvs_handle_t handle;
    if (nvs_open(BB_NV_CONFIG_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return false;
    uint8_t val = 0;
    nvs_get_u8(handle, "ota_skip", &val);
    nvs_close(handle);
    return val != 0;
}

bb_err_t bb_nv_config_set_ota_skip_check(bool skip)
{
    nvs_handle_t handle;
    bb_err_t err = nvs_open(BB_NV_CONFIG_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(handle, "ota_skip", skip ? 1 : 0);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

bb_err_t bb_nv_set_u8(const char *ns, const char *key, uint8_t value)
{
    if (ns == NULL || key == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    bb_err_t err = nvs_open(ns, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    err = nvs_set_u8(handle, key, value);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    return err;
}

bb_err_t bb_nv_set_u32(const char *ns, const char *key, uint32_t value)
{
    if (ns == NULL || key == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    bb_err_t err = nvs_open(ns, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    err = nvs_set_u32(handle, key, value);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    return err;
}

bb_err_t bb_nv_set_str(const char *ns, const char *key, const char *value)
{
    if (ns == NULL || key == NULL || value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    bb_err_t err = nvs_open(ns, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    err = nvs_set_str(handle, key, value);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    return err;
}

bb_err_t bb_nv_get_u8(const char *ns, const char *key, uint8_t *out, uint8_t fallback)
{
    if (ns == NULL || key == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    bb_err_t err = nvs_open(ns, NVS_READONLY, &handle);

    if (err == ESP_ERR_NVS_NOT_FOUND || err == ESP_ERR_NVS_NOT_INITIALIZED) {
        *out = fallback;
        return ESP_OK;
    }

    if (err != ESP_OK) {
        return err;
    }

    err = nvs_get_u8(handle, key, out);
    nvs_close(handle);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        *out = fallback;
        return ESP_OK;
    }

    return err;
}

bb_err_t bb_nv_get_u32(const char *ns, const char *key, uint32_t *out, uint32_t fallback)
{
    if (ns == NULL || key == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    bb_err_t err = nvs_open(ns, NVS_READONLY, &handle);

    if (err == ESP_ERR_NVS_NOT_FOUND || err == ESP_ERR_NVS_NOT_INITIALIZED) {
        *out = fallback;
        return ESP_OK;
    }

    if (err != ESP_OK) {
        return err;
    }

    err = nvs_get_u32(handle, key, out);
    nvs_close(handle);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        *out = fallback;
        return ESP_OK;
    }

    return err;
}

bb_err_t bb_nv_get_str(const char *ns, const char *key, char *buf, size_t len, const char *fallback)
{
    if (ns == NULL || key == NULL || buf == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    bb_err_t err = nvs_open(ns, NVS_READONLY, &handle);

    if (err == ESP_ERR_NVS_NOT_FOUND || err == ESP_ERR_NVS_NOT_INITIALIZED) {
        if (fallback == NULL) {
            buf[0] = '\0';
        } else {
            strlcpy(buf, fallback, len);
        }
        return ESP_OK;
    }

    if (err != ESP_OK) {
        return err;
    }

    size_t buf_len = len;
    err = nvs_get_str(handle, key, buf, &buf_len);
    nvs_close(handle);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        if (fallback == NULL) {
            buf[0] = '\0';
        } else {
            strlcpy(buf, fallback, len);
        }
        return ESP_OK;
    }

    return err;
}

bb_err_t bb_nv_erase(const char *ns, const char *key)
{
    if (ns == NULL || key == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    bb_err_t err = nvs_open(ns, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    err = nvs_erase_key(handle, key);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    return err;
}

#else

bb_err_t bb_nv_set_u8(const char *ns, const char *key, uint8_t value)
{
    if (ns == NULL || key == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

bb_err_t bb_nv_set_u32(const char *ns, const char *key, uint32_t value)
{
    if (ns == NULL || key == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

bb_err_t bb_nv_set_str(const char *ns, const char *key, const char *value)
{
    if (ns == NULL || key == NULL || value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

bb_err_t bb_nv_get_u8(const char *ns, const char *key, uint8_t *out, uint8_t fallback)
{
    if (ns == NULL || key == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = fallback;
    return ESP_OK;
}

bb_err_t bb_nv_get_u32(const char *ns, const char *key, uint32_t *out, uint32_t fallback)
{
    if (ns == NULL || key == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = fallback;
    return ESP_OK;
}

bb_err_t bb_nv_get_str(const char *ns, const char *key, char *buf, size_t len, const char *fallback)
{
    if (ns == NULL || key == NULL || buf == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (fallback == NULL) {
        buf[0] = '\0';
    } else {
        strlcpy(buf, fallback, len);
    }
    return ESP_OK;
}

bb_err_t bb_nv_erase(const char *ns, const char *key)
{
    if (ns == NULL || key == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

#endif

const char *bb_nv_config_wifi_ssid(void) { return s_config.wifi_ssid; }
const char *bb_nv_config_wifi_pass(void) { return s_config.wifi_pass; }
bool bb_nv_config_display_enabled(void) { return s_config.display_en != 0; }
