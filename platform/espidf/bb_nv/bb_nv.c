#include "bb_nv.h"
#include "bb_log.h"
#include "bb_manifest.h"
#include "bb_nv_wifi_pending.h"
#include "bb_str.h"
#include "bb_storage_nvs.h"
#include <stdbool.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_attr.h"
#include "bb_nv_creds_mirror.h"
#endif

#define BB_NV_KEY_WIFI_SSID         "wifi_ssid"
#define BB_NV_KEY_WIFI_PASS         "wifi_pass"
#define BB_NV_KEY_TIMEZONE          "timezone"
#define BB_NV_KEY_MDNS_EN           "mdns_en"
#define BB_NV_KEY_UPDATE_CHECK_EN   "update_check_en"
#define BB_NV_KEY_DISPLAY_EN        "display_en"
#define BB_NV_KEY_PROVISIONED       "provisioned"
#define BB_NV_KEY_WIFI_SSID_P       "wifi_ssid_p"
#define BB_NV_KEY_WIFI_PASS_P       "wifi_pass_p"
#define BB_NV_KEY_WIFI_TRY          "wifi_try"

#define BB_NV_TIMEZONE_MAX_LEN 65  /* 64 chars + NUL */

static const char *TAG_NV = "bb_nv";

/* RTC creds backup — B1-242.
 * s_creds_mirror survives software reset/panic (RTC_NOINIT_ATTR).
 * s_nvs_was_erased and s_creds_restored are plain BSS (per-boot flags). */
#ifdef ESP_PLATFORM
static bool s_nvs_was_erased;
static bool s_creds_restored;
#if defined(CONFIG_BB_NV_CREDS_RTC_BACKUP)
static RTC_NOINIT_ATTR bb_nv_creds_mirror_t s_creds_mirror;
#endif
#endif

static const bb_manifest_nv_t s_bb_cfg_keys[] = {
    {
        .key              = BB_NV_KEY_WIFI_SSID,
        .type             = "str",
        .default_         = NULL,
        .max_len          = 32,
        .desc             = "WiFi SSID",
        .reboot_required  = true,
        .provisioning_only = true,
    },
    {
        .key              = BB_NV_KEY_WIFI_PASS,
        .type             = "str",
        .default_         = NULL,
        .max_len          = 63,
        .desc             = "WiFi password",
        .reboot_required  = true,
        .provisioning_only = true,
    },
    {
        .key              = BB_NV_KEY_TIMEZONE,
        .type             = "str",
        .default_         = NULL,
        .max_len          = 64,
        .desc             = "POSIX timezone string (e.g. EST5EDT,M3.2.0,M11.1.0); empty = UTC",
        .reboot_required  = false,
        .provisioning_only = false,
    },
    {
        .key              = BB_NV_KEY_MDNS_EN,
        .type             = "bool",
        .default_         = "true",
        .max_len          = 0,
        .desc             = "Enable mDNS service advertisement",
        .reboot_required  = true,
        .provisioning_only = false,
    },
    {
        .key              = BB_NV_KEY_UPDATE_CHECK_EN,
        .type             = "bool",
        .default_         = "true",
        .max_len          = 0,
        .desc             = "Enable periodic firmware update check",
        .reboot_required  = false,
        .provisioning_only = false,
    },
    {
        .key              = BB_NV_KEY_DISPLAY_EN,
        .type             = "bool",
        .default_         = "true",
        .max_len          = 0,
        .desc             = "Enable display backend",
        .reboot_required  = true,
        .provisioning_only = false,
    },
    {
        .key              = BB_NV_KEY_PROVISIONED,
        .type             = "bool",
        .default_         = "false",
        .max_len          = 0,
        .desc             = "Provisioning completed flag (set after first successful config save; "
                            "pre-seed via direct NVS write to skip AP-mode boot, e.g. for "
                            "factory flash workflows)",
        .reboot_required  = true,
        .provisioning_only = false,
    },
};

static struct {
    char wifi_ssid[32];
    char wifi_pass[64];
    char timezone[BB_NV_TIMEZONE_MAX_LEN];
    uint8_t display_en;
    uint8_t mdns_en;
    uint8_t update_check_en;
} s_config;

/* Pending wifi creds — staged separately from live creds.
 * NEVER loaded into s_config.wifi_ssid/wifi_pass; kept isolated so the
 * RTC-restore gate (s_config.wifi_ssid[0]=='\0') is not affected. */
#ifdef ESP_PLATFORM
static struct {
    char ssid[32]; /* BB_WIFI_PENDING_SSID_MAX+1 */
    char pass[64]; /* BB_WIFI_PENDING_PASS_MAX+1 */
} s_pending;
#endif

// Helper to load a string from NVS with fallback (ESP only)
#ifdef ESP_PLATFORM
static const char *TAG = "nv_config";

static void load_str(nvs_handle_t handle, const char *key, char *buf, size_t buf_size, const char *fallback)
{
    size_t len = buf_size;
    if (nvs_get_str(handle, key, buf, &len) != ESP_OK) {
        bb_strlcpy(buf, fallback, buf_size);
    }
}

static bb_err_t nv_config_set_u8(const char *key, uint8_t val)
{
    nvs_handle_t handle;
    bb_err_t err = nvs_open(BB_NV_CONFIG_NVS_NS, NVS_READWRITE, &handle);
    if (err != BB_OK) return err;
    err = nvs_set_u8(handle, key, val);
    if (err == BB_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

static bb_err_t nv_config_set_str(const char *key, const char *val)
{
    nvs_handle_t handle;
    bb_err_t err = nvs_open(BB_NV_CONFIG_NVS_NS, NVS_READWRITE, &handle);
    if (err != BB_OK) return err;
    err = nvs_set_str(handle, key, val);
    if (err == BB_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}
#endif

bb_err_t bb_nv_config_init(void)
{
#ifdef ESP_PLATFORM
    bb_err_t flash_err = bb_nv_flash_init();
    if (flash_err != BB_OK) return flash_err;
    nvs_handle_t handle;
    bb_err_t err = nvs_open(BB_NV_CONFIG_NVS_NS, NVS_READONLY, &handle);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        bb_log_i(TAG, "no config in NVS");
        memset(&s_config, 0, sizeof(s_config));
        s_config.display_en = 1;  // default: display on
        s_config.mdns_en = 1;  // default: mdns on
        s_config.update_check_en = 1;  // default: update check on
        return BB_OK;
    }

    if (err != BB_OK) {
        return err;
    }

    load_str(handle, BB_NV_KEY_WIFI_SSID, s_config.wifi_ssid, sizeof(s_config.wifi_ssid), "");
    load_str(handle, BB_NV_KEY_WIFI_PASS, s_config.wifi_pass, sizeof(s_config.wifi_pass), "");
    load_str(handle, BB_NV_KEY_TIMEZONE, s_config.timezone, sizeof(s_config.timezone), "");

#if defined(CONFIG_BB_NV_CREDS_RTC_BACKUP)
    /* Restore+heal: if NVS has no creds but the RTC mirror is valid, recover
     * them. Close the read-only handle and reopen read-write so the heal
     * write-back goes through the same handle lifecycle as the rest of init.
     * Avoids a re-entrant bb_nv_config_set_wifi call (which would re-pack the
     * mirror redundantly) and keeps a single commit for both credential keys. */
    {
        bool handle_open = true;
        if (s_config.wifi_ssid[0] == '\0' &&
            bb_nv_creds_mirror_valid(&s_creds_mirror) &&
            s_creds_mirror.ssid[0] != '\0') {
            nvs_close(handle);
            handle_open = false;
            bb_err_t rw_err = nvs_open(BB_NV_CONFIG_NVS_NS, NVS_READWRITE, &handle);
            if (rw_err == BB_OK) {
                handle_open = true;
                bb_strlcpy(s_config.wifi_ssid, s_creds_mirror.ssid, sizeof(s_config.wifi_ssid));
                bb_strlcpy(s_config.wifi_pass, s_creds_mirror.pass, sizeof(s_config.wifi_pass));
                nvs_set_str(handle, BB_NV_KEY_WIFI_SSID, s_config.wifi_ssid);
                nvs_set_str(handle, BB_NV_KEY_WIFI_PASS, s_config.wifi_pass);
                if (s_creds_mirror.provisioned) {
                    nvs_set_u8(handle, BB_NV_KEY_PROVISIONED, 1);
                }
                nvs_commit(handle);
            } else {
                /* Reopen failed — apply creds in-memory; NVS heals on next write. */
                bb_strlcpy(s_config.wifi_ssid, s_creds_mirror.ssid, sizeof(s_config.wifi_ssid));
                bb_strlcpy(s_config.wifi_pass, s_creds_mirror.pass, sizeof(s_config.wifi_pass));
            }
            s_creds_restored = true;
            bb_log_w(TAG, "creds restored from RTC backup");
        }
        /* Load remaining booleans only if handle is still open. */
        if (handle_open) {
            if (nvs_get_u8(handle, BB_NV_KEY_DISPLAY_EN, &s_config.display_en) != ESP_OK) {
                s_config.display_en = 1;
            }
            if (nvs_get_u8(handle, BB_NV_KEY_MDNS_EN, &s_config.mdns_en) != ESP_OK) {
                s_config.mdns_en = 1;
            }
            if (nvs_get_u8(handle, BB_NV_KEY_UPDATE_CHECK_EN, &s_config.update_check_en) != ESP_OK) {
                s_config.update_check_en = 1;
            }
            nvs_close(handle);
        } else {
            s_config.display_en = 1;
            s_config.mdns_en = 1;
            s_config.update_check_en = 1;
        }
    }
#else

    if (nvs_get_u8(handle, BB_NV_KEY_DISPLAY_EN, &s_config.display_en) != ESP_OK) {
        s_config.display_en = 1;  // default: display on
    }

    if (nvs_get_u8(handle, BB_NV_KEY_MDNS_EN, &s_config.mdns_en) != ESP_OK) {
        s_config.mdns_en = 1;  // default: mdns on
    }

    if (nvs_get_u8(handle, BB_NV_KEY_UPDATE_CHECK_EN, &s_config.update_check_en) != ESP_OK) {
        s_config.update_check_en = 1;  // default: update check on
    }

    nvs_close(handle);
#endif

    /* Load pending wifi creds cache — separate from live load; must not affect
     * the s_config.wifi_ssid[0]=='\0' RTC-restore gate above. */
    {
        nvs_handle_t ph;
        memset(&s_pending, 0, sizeof(s_pending));
        bb_err_t perr = nvs_open(BB_NV_CONFIG_NVS_NS, NVS_READONLY, &ph);
        if (perr == BB_OK) {
            load_str(ph, BB_NV_KEY_WIFI_SSID_P, s_pending.ssid, sizeof(s_pending.ssid), "");
            load_str(ph, BB_NV_KEY_WIFI_PASS_P, s_pending.pass, sizeof(s_pending.pass), "");
            nvs_close(ph);
        }
    }

    bb_log_i(TAG, "config loaded");
#else
    // Native build: no NVS, all fields empty/zero
    memset(&s_config, 0, sizeof(s_config));
    s_config.display_en = 1;
    s_config.mdns_en = 1;
    s_config.update_check_en = 1;
#endif
    return BB_OK;
}

bb_err_t bb_nv_config_manifest_init(void)
{
    return bb_manifest_register_nv(BB_NV_CONFIG_NVS_NS, s_bb_cfg_keys,
                                   sizeof(s_bb_cfg_keys) / sizeof(s_bb_cfg_keys[0]));
}

#ifdef ESP_PLATFORM
bb_err_t bb_nv_flash_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        bb_log_e(TAG, "NVS erased on corruption — creds may be lost");
        s_nvs_was_erased = true;
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

bool bb_nv_config_is_provisioned(void)
{
    nvs_handle_t handle;
    bb_err_t err = nvs_open(BB_NV_CONFIG_NVS_NS, NVS_READONLY, &handle);

    if (err != ESP_OK) {
        return false;
    }

    uint8_t value = 0;
    err = nvs_get_u8(handle, BB_NV_KEY_PROVISIONED, &value);
    nvs_close(handle);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return false;
    }

    return value == 1;
}

bb_err_t bb_nv_config_set_provisioned(void)
{
    nvs_handle_t handle;
    bb_err_t err = nvs_open(BB_NV_CONFIG_NVS_NS, NVS_READWRITE, &handle);

    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_u8(handle, BB_NV_KEY_PROVISIONED, 1);
    if (err == BB_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

#if defined(CONFIG_BB_NV_CREDS_RTC_BACKUP)
    if (err == BB_OK) {
        bb_nv_creds_mirror_pack(&s_creds_mirror, s_config.wifi_ssid, s_config.wifi_pass, 1);
    }
#endif

    return err;
}

bb_err_t bb_nv_config_set_wifi(const char *ssid, const char *pass)
{
    bb_err_t err = nv_config_set_str(BB_NV_KEY_WIFI_SSID, ssid);
    if (err == BB_OK) err = nv_config_set_str(BB_NV_KEY_WIFI_PASS, pass);
    if (err == BB_OK) {
        bb_strlcpy(s_config.wifi_ssid, ssid, sizeof(s_config.wifi_ssid));
        bb_strlcpy(s_config.wifi_pass, pass, sizeof(s_config.wifi_pass));
#if defined(CONFIG_BB_NV_CREDS_RTC_BACKUP)
        uint8_t prov = bb_nv_config_is_provisioned() ? 1 : 0;
        bb_nv_creds_mirror_pack(&s_creds_mirror, s_config.wifi_ssid, s_config.wifi_pass, prov);
#endif
    }
    return err;
}

bb_err_t bb_nv_config_set_wifi_pending(const char *ssid, const char *pass)
{
    bb_err_t verr = bb_wifi_pending_validate(ssid, pass);
    if (verr != BB_OK) return verr;

    const char *p = pass ? pass : "";
    bb_nv_batch_t batch;
    bb_err_t err = bb_nv_batch_begin(&batch, BB_NV_CONFIG_NVS_NS);
    if (err != BB_OK) return err;
    bb_nv_batch_set_str(&batch, BB_NV_KEY_WIFI_SSID_P, ssid);
    bb_nv_batch_set_str(&batch, BB_NV_KEY_WIFI_PASS_P, p);
    bb_nv_batch_set_u8(&batch, BB_NV_KEY_WIFI_TRY, 1);
    err = bb_nv_batch_commit(&batch);
    if (err == BB_OK) {
        bb_strlcpy(s_pending.ssid, ssid, sizeof(s_pending.ssid));
        bb_strlcpy(s_pending.pass, p,    sizeof(s_pending.pass));
    }
    return err;
}

bool bb_nv_config_wifi_pending_active(void)
{
    uint8_t try_flag = 0;
    nvs_handle_t h;
    if (nvs_open(BB_NV_CONFIG_NVS_NS, NVS_READONLY, &h) == BB_OK) {
        nvs_get_u8(h, BB_NV_KEY_WIFI_TRY, &try_flag);
        nvs_close(h);
    }
    return bb_wifi_pending_decide(try_flag, s_pending.ssid) == BB_WIFI_PENDING_TRY;
}

const char *bb_nv_config_wifi_pending_ssid(void)
{
    return s_pending.ssid;
}

const char *bb_nv_config_wifi_pending_pass(void)
{
    return s_pending.pass;
}

bb_err_t bb_nv_config_commit_wifi_pending(void)
{
    if (s_pending.ssid[0] == '\0') return BB_ERR_INVALID_STATE;

    /* One batched transaction: promote pending -> live, erase pending keys. */
    nvs_handle_t h;
    bb_err_t err = nvs_open(BB_NV_CONFIG_NVS_NS, NVS_READWRITE, &h);
    if (err != BB_OK) return err;

    err = nvs_set_str(h, BB_NV_KEY_WIFI_SSID, s_pending.ssid);
    if (err == BB_OK) err = nvs_set_str(h, BB_NV_KEY_WIFI_PASS, s_pending.pass);
    if (err == BB_OK) {
        /* erase pending keys — absent key is benign */
        bb_err_t e;
        e = nvs_erase_key(h, BB_NV_KEY_WIFI_TRY);
        if (e == ESP_ERR_NVS_NOT_FOUND) e = BB_OK;
        if (e != BB_OK) err = e;
    }
    if (err == BB_OK) {
        bb_err_t e;
        e = nvs_erase_key(h, BB_NV_KEY_WIFI_SSID_P);
        if (e == ESP_ERR_NVS_NOT_FOUND) e = BB_OK;
        if (e != BB_OK) err = e;
    }
    if (err == BB_OK) {
        bb_err_t e;
        e = nvs_erase_key(h, BB_NV_KEY_WIFI_PASS_P);
        if (e == ESP_ERR_NVS_NOT_FOUND) e = BB_OK;
        if (e != BB_OK) err = e;
    }
    if (err == BB_OK) err = nvs_commit(h);
    nvs_close(h);

    if (err == BB_OK) {
        bb_strlcpy(s_config.wifi_ssid, s_pending.ssid, sizeof(s_config.wifi_ssid));
        bb_strlcpy(s_config.wifi_pass, s_pending.pass, sizeof(s_config.wifi_pass));
        memset(&s_pending, 0, sizeof(s_pending));
#if defined(CONFIG_BB_NV_CREDS_RTC_BACKUP)
        uint8_t prov = bb_nv_config_is_provisioned() ? 1 : 0;
        bb_nv_creds_mirror_pack(&s_creds_mirror, s_config.wifi_ssid, s_config.wifi_pass, prov);
#endif
    }
    return err;
}

bb_err_t bb_nv_config_clear_wifi_pending(void)
{
    nvs_handle_t h;
    bb_err_t err = nvs_open(BB_NV_CONFIG_NVS_NS, NVS_READWRITE, &h);
    if (err != BB_OK) return err;

    bb_err_t e;
    e = nvs_erase_key(h, BB_NV_KEY_WIFI_SSID_P);
    if (e == ESP_ERR_NVS_NOT_FOUND) e = BB_OK;
    if (e != BB_OK) err = e;

    if (err == BB_OK) {
        e = nvs_erase_key(h, BB_NV_KEY_WIFI_PASS_P);
        if (e == ESP_ERR_NVS_NOT_FOUND) e = BB_OK;
        if (e != BB_OK) err = e;
    }
    if (err == BB_OK) {
        e = nvs_erase_key(h, BB_NV_KEY_WIFI_TRY);
        if (e == ESP_ERR_NVS_NOT_FOUND) e = BB_OK;
        if (e != BB_OK) err = e;
    }
    if (err == BB_OK) err = nvs_commit(h);
    nvs_close(h);

    if (err == BB_OK) {
        memset(&s_pending, 0, sizeof(s_pending));
    }
    return err;
}

bb_err_t bb_nv_config_set_timezone(const char *tz)
{
    const char *t = (tz && tz[0] != '\0') ? tz : "";
    if (strlen(t) >= BB_NV_TIMEZONE_MAX_LEN) return BB_ERR_INVALID_ARG;
    bb_err_t err = nv_config_set_str(BB_NV_KEY_TIMEZONE, t);
    if (err == BB_OK) bb_strlcpy(s_config.timezone, t, sizeof(s_config.timezone));
    return err;
}

bb_err_t bb_nv_config_set_display_enabled(bool en)
{
    bb_err_t err = nv_config_set_u8(BB_NV_KEY_DISPLAY_EN, en ? 1 : 0);
    if (err == BB_OK) s_config.display_en = en ? 1 : 0;
    return err;
}

bb_err_t bb_nv_config_set_mdns_enabled(bool en)
{
    bb_err_t err = nv_config_set_u8(BB_NV_KEY_MDNS_EN, en ? 1 : 0);
    if (err == BB_OK) s_config.mdns_en = en ? 1 : 0;
    return err;
}

bb_err_t bb_nv_config_set_update_check_enabled(bool en)
{
    bb_err_t err = nv_config_set_u8(BB_NV_KEY_UPDATE_CHECK_EN, en ? 1 : 0);
    if (err == BB_OK) s_config.update_check_en = en ? 1 : 0;
    return err;
}

bb_err_t bb_nv_config_clear_provisioned(void)
{
    nvs_handle_t handle;
    bb_err_t err = nvs_open(BB_NV_CONFIG_NVS_NS, NVS_READWRITE, &handle);
    if (err != BB_OK) return err;
    err = nvs_erase_key(handle, BB_NV_KEY_PROVISIONED);
    if (err == ESP_ERR_NVS_NOT_FOUND) err = BB_OK;
    if (err == BB_OK) err = nvs_commit(handle);
    nvs_close(handle);
#if defined(CONFIG_BB_NV_CREDS_RTC_BACKUP)
    if (err == BB_OK) {
        /* Deliberate clear — invalidate mirror so it can't resurrect the creds. */
        memset(&s_creds_mirror, 0, sizeof(s_creds_mirror));
    }
#endif
    return err;
}

bb_err_t bb_nv_config_clear_wifi(void)
{
    nvs_handle_t handle;
    bb_err_t err = nvs_open(BB_NV_CONFIG_NVS_NS, NVS_READWRITE, &handle);
    if (err != BB_OK) return err;
    err = nvs_erase_key(handle, "wifi_ssid");
    if (err == ESP_ERR_NVS_NOT_FOUND) err = BB_OK;
    if (err == BB_OK) {
        err = nvs_erase_key(handle, "wifi_pass");
        if (err == ESP_ERR_NVS_NOT_FOUND) err = BB_OK;
    }
    if (err == BB_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err == BB_OK) {
        s_config.wifi_ssid[0] = '\0';
        s_config.wifi_pass[0] = '\0';
#if defined(CONFIG_BB_NV_CREDS_RTC_BACKUP)
        /* Deliberate factory-reset — invalidate mirror so it can't resurrect the creds. */
        memset(&s_creds_mirror, 0, sizeof(s_creds_mirror));
#endif
    }
    return err;
}

bool bb_nv_config_ota_skip_check(void)
{
    nvs_handle_t handle;
    if (nvs_open(BB_NV_CONFIG_NVS_NS, NVS_READONLY, &handle) != BB_OK) return false;
    uint8_t val = 0;
    nvs_get_u8(handle, "ota_skip", &val);
    nvs_close(handle);
    return val != 0;
}

bb_err_t bb_nv_config_set_ota_skip_check(bool skip)
{
    nvs_handle_t handle;
    bb_err_t err = nvs_open(BB_NV_CONFIG_NVS_NS, NVS_READWRITE, &handle);
    if (err != BB_OK) return err;
    err = nvs_set_u8(handle, "ota_skip", skip ? 1 : 0);
    if (err == BB_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

// bb_nv_set_u8/set_u16/set_u32/set_str/get_u8/get_u16/get_u32/get_str/erase/
// erase_namespace/exists are thin forwarders to bb_storage_nvs's typed
// accessors (components/bb_storage_nvs) — the NVS access logic itself
// (nvs_open/get/set/erase-per-call, type-mismatch handling) moved there
// verbatim; on-flash format is unchanged (B1: bb_storage_nvs PR2).
bb_err_t bb_nv_set_u8(const char *ns, const char *key, uint8_t value)
{
    return bb_storage_nvs_set_u8(ns, key, value);
}

bb_err_t bb_nv_set_u16(const char *ns, const char *key, uint16_t value)
{
    return bb_storage_nvs_set_u16(ns, key, value);
}

bb_err_t bb_nv_set_u32(const char *ns, const char *key, uint32_t value)
{
    return bb_storage_nvs_set_u32(ns, key, value);
}

bb_err_t bb_nv_set_str(const char *ns, const char *key, const char *value)
{
    return bb_storage_nvs_set_str(ns, key, value);
}

bb_err_t bb_nv_get_u8(const char *ns, const char *key, uint8_t *out, uint8_t fallback)
{
    return bb_storage_nvs_get_u8(ns, key, out, fallback);
}

bb_err_t bb_nv_get_u16(const char *ns, const char *key, uint16_t *out, uint16_t fallback)
{
    return bb_storage_nvs_get_u16(ns, key, out, fallback);
}

bb_err_t bb_nv_get_u32(const char *ns, const char *key, uint32_t *out, uint32_t fallback)
{
    return bb_storage_nvs_get_u32(ns, key, out, fallback);
}

bb_err_t bb_nv_get_str(const char *ns, const char *key, char *buf, size_t len, const char *fallback)
{
    return bb_storage_nvs_get_str(ns, key, buf, len, fallback);
}

bb_err_t bb_nv_erase(const char *ns, const char *key)
{
    return bb_storage_nvs_erase(ns, key);
}

bb_err_t bb_nv_erase_namespace(const char *ns)
{
    return bb_storage_nvs_erase_namespace(ns);
}

bool bb_nv_exists(const char *ns, const char *key)
{
    return bb_storage_nvs_exists(ns, key);
}

/* -------- batched setters -------- */

bb_err_t bb_nv_batch_begin(bb_nv_batch_t *batch, const char *ns)
{
    if (batch == NULL || ns == NULL) return BB_ERR_INVALID_ARG;
    batch->_impl = 0;
    batch->_err = BB_OK;
    batch->_open = 0;
    bb_strlcpy(batch->_ns, ns, sizeof(batch->_ns));
    nvs_handle_t handle;
    bb_err_t err = nvs_open(ns, NVS_READWRITE, &handle);
    if (err != BB_OK) {
        batch->_err = err;
        return err;
    }
    batch->_impl = (uintptr_t)handle;
    batch->_open = 1;
    return BB_OK;
}

/* Common entry guard for batch setters: rejects null args, unopened handles,
 * and sticky-poisoned batches (where a previous set already failed). */
static bb_err_t batch_check(bb_nv_batch_t *batch, const char *key)
{
    if (batch == NULL || key == NULL) return BB_ERR_INVALID_ARG;
    if (!batch->_open) return BB_ERR_INVALID_STATE;
    return batch->_err;
}

#define BB_NV_BATCH_SET_INT(WIDTH)                                                          \
    bb_err_t bb_nv_batch_set_u##WIDTH(bb_nv_batch_t *batch, const char *key,                \
                                      uint##WIDTH##_t value)                                \
    {                                                                                       \
        bb_err_t pre = batch_check(batch, key);                                             \
        if (pre != BB_OK) return pre;                                                       \
        nvs_handle_t h = (nvs_handle_t)batch->_impl;                                        \
        bb_err_t err = nvs_set_u##WIDTH(h, key, value);                                     \
        if (err == ESP_ERR_NVS_TYPE_MISMATCH) {                                             \
            bb_log_w(TAG_NV, "type mismatch on batch set '%s/%s', rewriting",               \
                     batch->_ns, key);                                                      \
            (void)nvs_erase_key(h, key);                                                    \
            err = nvs_set_u##WIDTH(h, key, value);                                          \
        }                                                                                   \
        if (err != BB_OK) batch->_err = err;                                                \
        return err;                                                                         \
    }
BB_NV_BATCH_SET_INT(8)
BB_NV_BATCH_SET_INT(16)
BB_NV_BATCH_SET_INT(32)
#undef BB_NV_BATCH_SET_INT

bb_err_t bb_nv_batch_set_str(bb_nv_batch_t *batch, const char *key, const char *value)
{
    bb_err_t pre = batch_check(batch, key);
    if (pre != BB_OK) return pre;
    if (value == NULL) return BB_ERR_INVALID_ARG;
    nvs_handle_t h = (nvs_handle_t)batch->_impl;
    bb_err_t err = nvs_set_str(h, key, value);
    if (err != BB_OK) batch->_err = err;
    return err;
}

bb_err_t bb_nv_batch_commit(bb_nv_batch_t *batch)
{
    if (batch == NULL) return BB_ERR_INVALID_ARG;
    if (!batch->_open) return batch->_err;
    nvs_handle_t h = (nvs_handle_t)batch->_impl;
    bb_err_t err = batch->_err;
    if (err == BB_OK) err = nvs_commit(h);
    nvs_close(h);
    batch->_open = 0;
    batch->_impl = 0;
    if (err != BB_OK && batch->_err == BB_OK) batch->_err = err;
    return err;
}

bb_err_t bb_nv_config_factory_reset(void)
{
    bb_log_i(TAG, "factory reset: erasing NVS partition");
    bb_err_t err = nvs_flash_erase();
    if (err != BB_OK) {
        bb_log_e(TAG, "factory reset: nvs_flash_erase failed: %d", err);
        return err;
    }
    /* Invalidate the RTC mirror so the restore-heal path on next boot does NOT
     * re-populate credentials. Without this, the mirror's valid CRC would cause
     * bb_nv_config_init to copy creds back into NVS, silently defeating the reset. */
#if defined(CONFIG_BB_NV_CREDS_RTC_BACKUP)
    memset(&s_creds_mirror, 0, sizeof(s_creds_mirror));
    /* magic is now 0 → bb_nv_creds_mirror_valid() returns false → no heal. */
#endif
    /* Clear in-RAM cache so callers see the wiped state immediately. */
    memset(&s_config, 0, sizeof(s_config));
    s_config.display_en = 1;
    s_config.mdns_en = 1;
    s_config.update_check_en = 1;
    memset(&s_pending, 0, sizeof(s_pending));
    bb_log_i(TAG, "factory reset: done");
    return BB_OK;
}

/* Query API — both symbols always present under ESP_PLATFORM so consumers
 * link unconditionally regardless of CONFIG_BB_NV_CREDS_RTC_BACKUP. */
bool bb_nv_config_was_erased(void)
{
    return s_nvs_was_erased;
}

bool bb_nv_config_creds_restored(void)
{
#if defined(CONFIG_BB_NV_CREDS_RTC_BACKUP)
    return s_creds_restored;
#else
    return false;
#endif
}

#else

/* ---------------------------------------------------------------------------
 * Host in-memory string store for bb_nv_get_str / bb_nv_set_str.
 *
 * Provides real set/get round-trip semantics on host so components that read
 * NVS (e.g. bb_tls_creds) can be exercised in unit tests.  Integers remain
 * no-op stubs (tests that need integer NVS round-trips on host should be
 * promoted to ESP-IDF target tests).
 *
 * Capacity is intentionally small — enough for test scenarios.
 * --------------------------------------------------------------------------- */

#define BB_NV_HOST_STR_NS_MAX   16
#define BB_NV_HOST_STR_KEY_MAX  16
#define BB_NV_HOST_STR_VAL_MAX  4096
#define BB_NV_HOST_STR_ENTRIES  32

typedef struct {
    char ns [BB_NV_HOST_STR_NS_MAX];
    char key[BB_NV_HOST_STR_KEY_MAX];
    char val[BB_NV_HOST_STR_VAL_MAX];
    bool used;
} bb_nv_host_str_entry_t;

static bb_nv_host_str_entry_t s_str_store[BB_NV_HOST_STR_ENTRIES];

void bb_nv_host_str_store_reset(void)
{
    memset(s_str_store, 0, sizeof(s_str_store));
}

static bb_nv_host_str_entry_t *str_store_find(const char *ns, const char *key)
{
    for (int i = 0; i < BB_NV_HOST_STR_ENTRIES; i++) {
        if (s_str_store[i].used &&
            strncmp(s_str_store[i].ns,  ns,  BB_NV_HOST_STR_NS_MAX  - 1) == 0 &&
            strncmp(s_str_store[i].key, key, BB_NV_HOST_STR_KEY_MAX - 1) == 0) {
            return &s_str_store[i];
        }
    }
    return NULL;
}

bb_err_t bb_nv_set_u8(const char *ns, const char *key, uint8_t value)
{
    (void)value;
    if (ns == NULL || key == NULL) {
        return BB_ERR_INVALID_ARG;
    }
    return BB_OK;
}

bb_err_t bb_nv_set_u16(const char *ns, const char *key, uint16_t value)
{
    (void)value;
    if (ns == NULL || key == NULL) {
        return BB_ERR_INVALID_ARG;
    }
    return BB_OK;
}

bb_err_t bb_nv_set_u32(const char *ns, const char *key, uint32_t value)
{
    (void)value;
    if (ns == NULL || key == NULL) {
        return BB_ERR_INVALID_ARG;
    }
    return BB_OK;
}

bb_err_t bb_nv_set_str(const char *ns, const char *key, const char *value)
{
    if (ns == NULL || key == NULL || value == NULL) {
        return BB_ERR_INVALID_ARG;
    }
    /* Update existing entry if present */
    bb_nv_host_str_entry_t *e = str_store_find(ns, key);
    if (!e) {
        /* Find a free slot */
        for (int i = 0; i < BB_NV_HOST_STR_ENTRIES; i++) {
            if (!s_str_store[i].used) {
                e = &s_str_store[i];
                break;
            }
        }
    }
    if (!e) return BB_ERR_NO_SPACE; /* store full */
    bb_strlcpy(e->ns,  ns,  sizeof(e->ns));
    bb_strlcpy(e->key, key, sizeof(e->key));
    bb_strlcpy(e->val, value, sizeof(e->val));
    e->used = true;
    return BB_OK;
}

bb_err_t bb_nv_get_u8(const char *ns, const char *key, uint8_t *out, uint8_t fallback)
{
    if (ns == NULL || key == NULL || out == NULL) {
        return BB_ERR_INVALID_ARG;
    }
    *out = fallback;
    return BB_OK;
}

bb_err_t bb_nv_get_u16(const char *ns, const char *key, uint16_t *out, uint16_t fallback)
{
    if (ns == NULL || key == NULL || out == NULL) {
        return BB_ERR_INVALID_ARG;
    }
    *out = fallback;
    return BB_OK;
}

bb_err_t bb_nv_get_u32(const char *ns, const char *key, uint32_t *out, uint32_t fallback)
{
    if (ns == NULL || key == NULL || out == NULL) {
        return BB_ERR_INVALID_ARG;
    }
    *out = fallback;
    return BB_OK;
}

bb_err_t bb_nv_get_str(const char *ns, const char *key, char *buf, size_t len, const char *fallback)
{
    if (ns == NULL || key == NULL || buf == NULL || len == 0) {
        return BB_ERR_INVALID_ARG;
    }
    const bb_nv_host_str_entry_t *e = str_store_find(ns, key);
    if (e) {
        bb_strlcpy(buf, e->val, len);
        return BB_OK;
    }
    if (fallback == NULL) {
        buf[0] = '\0';
    } else {
        bb_strlcpy(buf, fallback, len);
    }
    return BB_OK;
}

bb_err_t bb_nv_erase(const char *ns, const char *key)
{
    if (ns == NULL || key == NULL) {
        return BB_ERR_INVALID_ARG;
    }
    bb_nv_host_str_entry_t *e = str_store_find(ns, key);
    if (e) {
        memset(e, 0, sizeof(*e));
    }
    return BB_OK;
}

bb_err_t bb_nv_erase_namespace(const char *ns)
{
    if (ns == NULL) {
        return BB_ERR_INVALID_ARG;
    }
    for (int i = 0; i < BB_NV_HOST_STR_ENTRIES; i++) {
        if (s_str_store[i].used &&
            strncmp(s_str_store[i].ns, ns, BB_NV_HOST_STR_NS_MAX - 1) == 0) {
            memset(&s_str_store[i], 0, sizeof(s_str_store[i]));
        }
    }
    return BB_OK;
}

bool bb_nv_exists(const char *ns, const char *key)
{
    if (ns == NULL || key == NULL) return false;
    const bb_nv_host_str_entry_t *e = str_store_find(ns, key);
    return (e != NULL && e->val[0] != '\0');
}

/* -------- batched setters (host stubs) -------- */

bb_err_t bb_nv_batch_begin(bb_nv_batch_t *batch, const char *ns)
{
    if (batch == NULL || ns == NULL) return BB_ERR_INVALID_ARG;
    batch->_impl = 0;
    batch->_err = BB_OK;
    batch->_open = 1;
    bb_strlcpy(batch->_ns, ns, sizeof(batch->_ns));
    return BB_OK;
}

#define BB_NV_BATCH_SET_STUB(WIDTH)                                                  \
    bb_err_t bb_nv_batch_set_u##WIDTH(bb_nv_batch_t *batch, const char *key,         \
                                      uint##WIDTH##_t value)                         \
    {                                                                                \
        (void)value;                                                                  \
        if (batch == NULL || key == NULL) return BB_ERR_INVALID_ARG;                 \
        if (!batch->_open) return BB_ERR_INVALID_STATE;                              \
        return batch->_err;                                                          \
    }
BB_NV_BATCH_SET_STUB(8)
BB_NV_BATCH_SET_STUB(16)
BB_NV_BATCH_SET_STUB(32)
#undef BB_NV_BATCH_SET_STUB

bb_err_t bb_nv_batch_set_str(bb_nv_batch_t *batch, const char *key, const char *value)
{
    if (batch == NULL || key == NULL || value == NULL) return BB_ERR_INVALID_ARG;
    if (!batch->_open) return BB_ERR_INVALID_STATE;
    return batch->_err;
}

bb_err_t bb_nv_batch_commit(bb_nv_batch_t *batch)
{
    if (batch == NULL) return BB_ERR_INVALID_ARG;
    batch->_open = 0;
    return batch->_err;
}

#endif /* !ESP_PLATFORM (host stubs for set/get/batch) */

// Host implementations of ESP-only setters (non-ESP)
#ifndef ESP_PLATFORM
static bool s_force_set_update_check_fail = false;
void bb_nv_config_host_force_set_update_check_fail(bool fail)
{
    s_force_set_update_check_fail = fail;
}

bb_err_t bb_nv_config_set_timezone(const char *tz)
{
    const char *t = (tz && tz[0] != '\0') ? tz : "";
    if (strlen(t) >= BB_NV_TIMEZONE_MAX_LEN) return BB_ERR_INVALID_ARG;
    bb_strlcpy(s_config.timezone, t, sizeof(s_config.timezone));
    return BB_OK;
}

bb_err_t bb_nv_config_set_display_enabled(bool en)
{
    s_config.display_en = en ? 1 : 0;
    return BB_OK;
}

bb_err_t bb_nv_config_set_update_check_enabled(bool en)
{
    if (s_force_set_update_check_fail) return BB_ERR_INVALID_STATE;
    s_config.update_check_en = en ? 1 : 0;
    return BB_OK;
}

bb_err_t bb_nv_config_factory_reset(void)
{
    /* Host implementation: clear in-memory config to defaults. No NVS or RTC
     * mirror exists on host, so we just zero the cache. This lets host tests
     * assert that factory reset clears credentials and resets flag state.
     *
     * hostname (and, longer-term, the wifi creds still read below) now live
     * in bb_settings' storage layer, not s_config. On real hardware,
     * bb_nv_config_factory_reset() (ESP_PLATFORM branch) erases the WHOLE
     * "bb_cfg" NVS partition via nvs_flash_erase(), which correctly wipes
     * bb_settings' hostname/wifi-cred keys too since they share that
     * namespace -- device behavior is correct there. This host stub only
     * zeroes bb_nv's own legacy s_config, so it does NOT clear a
     * host-seeded bb_settings hostname; factory-reset ownership for
     * bb_settings-owned fields is pending migration out of bb_nv. */
    memset(&s_config, 0, sizeof(s_config));
    s_config.display_en = 1;
    s_config.mdns_en = 1;
    s_config.update_check_en = 1;
    return BB_OK;
}
#endif

const char *bb_nv_config_wifi_ssid(void) { return s_config.wifi_ssid; }
const char *bb_nv_config_wifi_pass(void) { return s_config.wifi_pass; }
const char *bb_nv_config_timezone(void) { return s_config.timezone; }
bool bb_nv_config_display_enabled(void) { return s_config.display_en != 0; }
bool bb_nv_config_mdns_enabled(void) { return s_config.mdns_en != 0; }
bool bb_nv_config_update_check_enabled(void) { return s_config.update_check_en != 0; }
