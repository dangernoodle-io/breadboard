#include "bb_nv.h"
#include "bb_log.h"
#include "bb_manifest.h"
#include "bb_nv_namespaces.h"
#include "bb_settings.h"
#include "bb_str.h"
#include "bb_storage_nvs.h"
#include <stdbool.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_attr.h"
#include "bb_nv_creds_boot_decide.h"
#endif

#define BB_NV_KEY_WIFI_SSID         "wifi_ssid"
#define BB_NV_KEY_WIFI_PASS         "wifi_pass"
#define BB_NV_KEY_PROVISIONED       "provisioned"

static const char *TAG_NV = "bb_nv";

/* RTC creds backup — B1-242.
 * The mirror region itself now lives entirely behind bb_settings' RTC
 * mirror accessors (bb_settings_wifi_rtc_mirror_*, "rtc" bb_storage
 * backend) — bb_nv no longer owns a private RTC_NOINIT_ATTR region (B1: bb_nv
 * creds-cluster relocation). s_creds_restored is a plain BSS (per-boot) flag.
 * The "was NVS erased this boot" flag lives in bb_storage_nvs.c
 * (bb_storage_nvs_flash_was_erased) alongside the erase-and-retry logic it
 * tracks — see B1-840. */
#ifdef ESP_PLATFORM
static bool s_creds_restored;
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

#ifdef ESP_PLATFORM
static const char *TAG = "nv_config";
#endif

bb_err_t bb_nv_config_init(void)
{
#ifdef ESP_PLATFORM
    bb_err_t flash_err = bb_storage_nvs_flash_init();
    if (flash_err != BB_OK) return flash_err;

#if defined(CONFIG_BB_NV_CREDS_RTC_BACKUP)
    /* The heal-vs-seed DECISION is a pure function (bb_nv_creds_boot_decide,
     * components/bb_nv/src/) and is host-tested there -- see
     * test/test_host/test_bb_nv_creds_boot_decide.c for all 4 input
     * combinations plus bite-proof (RED-when-inverted) evidence. Everything
     * below this point is the NVS/RTC-mirror I/O for whichever action came
     * back; that I/O is espidf-only (B1-943/B1-516, coverage-invisible) and
     * rides on HW validation, NOT host coverage -- do not read the `make
     * coverage` gate as proof this I/O is exercised.
     *
     * requires=storage_rtc on this fn's // bbtool:init marker (bb_nv.h)
     * forces bb_storage_rtc's registration to run first in the same EARLY
     * tier -- without it, bb_settings_wifi_rtc_mirror_has_creds() would
     * silently read BB_ERR_NOT_FOUND/false and the heal action would never
     * be decided. */
    bb_nv_boot_action_t action = bb_nv_creds_boot_decide(bb_settings_wifi_has_creds(),
                                                          bb_settings_wifi_rtc_mirror_has_creds());

    if (action == BB_NV_BOOT_HEAL) {
        /* Restore: NVS has no live creds but the shared "rtc" mirror
         * (bb_settings_wifi_rtc_mirror_*, "rtc" bb_storage backend) does --
         * recover them by writing straight through bb_settings' live-creds
         * writer -- a single atomic bb_config_staged commit against the SAME
         * "bb_cfg"/wifi_ssid/wifi_pass NVS keys bb_nv used to write directly
         * (byte-compat unaffected by this relocation, see bb_settings.h). */
        char ssid[32] = {0};
        char pass[64] = {0};
        size_t ssid_len = 0, pass_len = 0;
        bb_err_t sret = bb_settings_wifi_rtc_mirror_ssid_get(ssid, sizeof(ssid), &ssid_len);
        bb_err_t pret = bb_settings_wifi_rtc_mirror_pass_get(pass, sizeof(pass), &pass_len);
        if (sret == BB_OK && pret == BB_OK && ssid[0] != '\0') {
            bb_err_t werr = bb_settings_wifi_set(ssid, pass);
            if (werr == BB_OK) {
                s_creds_restored = true;
                bb_log_w(TAG, "creds restored from RTC backup");
                if (bb_settings_wifi_rtc_mirror_provisioned_get()) {
                    bb_nv_config_set_provisioned();
                }
            } else {
                bb_log_e(TAG, "creds restore from RTC backup failed to persist: %d", (int)werr);
            }
        }
    } else if (action == BB_NV_BOOT_SEED) {
        /* Mirror-seed: proactively arm the recovery net on a freshly-flashed
         * provisioned board, rather than lazily on the next credential
         * write. Never overwrites an already-valid mirror (which may hold
         * in-flight pending-try state written by
         * bb_settings_wifi_pending_promote), and is idempotent across warm
         * resets (a valid mirror decides NONE on every subsequent boot; this
         * fires at most once, right after a cold boot with erased/never-
         * armed RTC memory). */
        char ssid[32] = {0};
        char pass[64] = {0};
        size_t ssid_len = 0, pass_len = 0;
        bb_err_t sret = bb_settings_wifi_ssid_get(ssid, sizeof(ssid), &ssid_len);
        bb_err_t pret = bb_settings_wifi_pass_get(pass, sizeof(pass), &pass_len);
        if (sret == BB_OK && pret == BB_OK) {
            bb_settings_wifi_rtc_mirror_write(ssid, pass);
        }
    }
#endif

    bb_log_i(TAG, "config loaded");
#endif
    return BB_OK;
}

bb_err_t bb_nv_config_manifest_init(void)
{
    return bb_manifest_register_nv(BB_NV_CONFIG_NVS_NS, s_bb_cfg_keys,
                                   sizeof(s_bb_cfg_keys) / sizeof(s_bb_cfg_keys[0]));
}

#ifdef ESP_PLATFORM
/* Thin forwarder — see bb_nv.h. The erase-and-retry logic itself now lives
 * in bb_storage_nvs_flash_init() (B1-840). */
bb_err_t bb_nv_flash_init(void)
{
    return bb_storage_nvs_flash_init();
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
        // Re-stamp the shared RTC mirror with the current live creds and
        // provisioned=1 (bb_settings_wifi_rtc_mirror_write always sets the
        // mirror's provisioned key to 1 -- see bb_settings.c) so a later
        // warm-reboot heal can trust the mirror's provisioned bit. Fail-open
        // on either read/write here -- same posture as every other mirror
        // touch in this file.
        char ssid[32] = {0};
        char pass[64] = {0};
        size_t ssid_len = 0, pass_len = 0;
        if (bb_settings_wifi_ssid_get(ssid, sizeof(ssid), &ssid_len) == BB_OK &&
            bb_settings_wifi_pass_get(pass, sizeof(pass), &pass_len) == BB_OK) {
            bb_settings_wifi_rtc_mirror_write(ssid, pass);
        }
    }
#endif

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
        /* Deliberate clear — invalidate the shared mirror so it can't
         * resurrect the creds (best-effort; an unregistered "rtc" backend is
         * fail-open here, same posture as bb_settings' own mirror writers). */
        bb_settings_wifi_rtc_mirror_clear();
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
#if defined(CONFIG_BB_NV_CREDS_RTC_BACKUP)
    if (err == BB_OK) {
        /* Deliberate clear — invalidate the shared mirror so it can't
         * resurrect the creds (best-effort, same posture as
         * bb_nv_config_clear_provisioned above). */
        bb_settings_wifi_rtc_mirror_clear();
    }
#endif
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

/* Query API — both symbols always present under ESP_PLATFORM so consumers
 * link unconditionally regardless of CONFIG_BB_NV_CREDS_RTC_BACKUP. */
bool bb_nv_config_was_erased(void)
{
    return bb_storage_nvs_flash_was_erased();
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

