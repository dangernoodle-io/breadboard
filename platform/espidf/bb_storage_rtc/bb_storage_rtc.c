#include "bb_storage_rtc.h"
#include "bb_storage_rtc_region.h"
#include "bb_storage.h"
#include "bb_log.h"

#include "esp_attr.h"

#include <stddef.h>
#include <string.h>

static const char *TAG = "bb_storage_rtc";

// Own, SEPARATE RTC no-init region from bb_nv's s_creds_mirror (PR3a is
// additive-only — see bb_storage_rtc.h's file header; the two regions
// coexist until PR3b relocates bb_nv's write/heal call sites onto this
// backend and deletes bb_nv's copy).
static RTC_NOINIT_ATTR bb_storage_rtc_region_t s_region;

void bb_storage_rtc_test_reset(void)
{
    memset(&s_region, 0, sizeof(s_region));
}

#ifdef BB_STORAGE_RTC_TESTING
bb_storage_rtc_region_t *bb_storage_rtc_region_for_test(void)
{
    return &s_region;
}
#endif

static bb_err_t rtc_get(void *impl, const bb_storage_addr_t *addr, void *buf, size_t cap, size_t *out_len)
{
    (void)impl;

    if (addr->key == NULL) {
        return BB_ERR_INVALID_ARG;
    }

    if (!bb_storage_rtc_region_valid(&s_region)) {
        // Cold boot / corrupt / version-mismatched region -- every key
        // reads as absent, regardless of stale bytes still in RTC memory.
        return BB_ERR_NOT_FOUND;
    }

    if (strcmp(addr->key, "ssid") == 0) {
        size_t len = strlen(s_region.ssid);
        *out_len = len;
        if (cap > 0) {
            memcpy(buf, s_region.ssid, len < cap ? len : cap);
        }
        return BB_OK;
    }

    if (strcmp(addr->key, "pass") == 0) {
        size_t len = strlen(s_region.pass);
        *out_len = len;
        if (cap > 0) {
            memcpy(buf, s_region.pass, len < cap ? len : cap);
        }
        return BB_OK;
    }

    if (strcmp(addr->key, "provisioned") == 0) {
        *out_len = sizeof(s_region.provisioned);
        if (cap > 0) {
            memcpy(buf, &s_region.provisioned, cap < sizeof(s_region.provisioned) ? cap : sizeof(s_region.provisioned));
        }
        return BB_OK;
    }

    return BB_ERR_NOT_FOUND;
}

static bb_err_t rtc_set(void *impl, const bb_storage_addr_t *addr, const void *buf, size_t len)
{
    (void)impl;

    if (addr->key == NULL) {
        return BB_ERR_INVALID_ARG;
    }

    if (!bb_storage_rtc_region_valid(&s_region)) {
        // Cold boot / corrupt / version-mismatched region: RTC_NOINIT memory
        // is undefined on true first power-up (not guaranteed zero), so a
        // partial single-key write would re-stamp magic/version/CRC over
        // still-garbage fields elsewhere in the region. Promote garbage ->
        // well-defined all-zero before writing the one field below.
        memset(&s_region, 0, sizeof(s_region));
    }

    if (strcmp(addr->key, "ssid") == 0) {
        if (len > sizeof(s_region.ssid) - 1) {
            bb_log_w(TAG, "ssid (%u bytes) exceeds max %u bytes",
                     (unsigned)len, (unsigned)sizeof(s_region.ssid) - 1);
            return BB_ERR_NO_SPACE;
        }
        if (len > 0) {
            memcpy(s_region.ssid, buf, len);
        }
        s_region.ssid[len] = '\0';
    } else if (strcmp(addr->key, "pass") == 0) {
        if (len > sizeof(s_region.pass) - 1) {
            bb_log_w(TAG, "pass (%u bytes) exceeds max %u bytes",
                     (unsigned)len, (unsigned)sizeof(s_region.pass) - 1);
            return BB_ERR_NO_SPACE;
        }
        if (len > 0) {
            memcpy(s_region.pass, buf, len);
        }
        s_region.pass[len] = '\0';
    } else if (strcmp(addr->key, "provisioned") == 0) {
        if (len != sizeof(s_region.provisioned)) {
            return BB_ERR_INVALID_ARG;
        }
        memcpy(&s_region.provisioned, buf, sizeof(s_region.provisioned));
    } else {
        return BB_ERR_INVALID_ARG;
    }

    // A single-key set leaves the whole region re-stamped and healable:
    // magic/version + a freshly-computed CRC over every field, not just the
    // one just written.
    s_region.magic   = BB_STORAGE_RTC_REGION_MAGIC;
    s_region.version = BB_STORAGE_RTC_REGION_VERSION;
    s_region.crc     = bb_storage_rtc_region_crc(&s_region);

    return BB_OK;
}

static bb_err_t rtc_erase(void *impl, const bb_storage_addr_t *addr)
{
    (void)impl;

    if (addr->key == NULL) {
        return BB_ERR_INVALID_ARG;
    }

    if (strcmp(addr->key, "ssid") != 0 &&
        strcmp(addr->key, "pass") != 0 &&
        strcmp(addr->key, "provisioned") != 0) {
        return BB_ERR_INVALID_ARG;
    }

    // Whole-region invalidate -- mirrors bb_nv's existing clear paths
    // (zero the region so bb_storage_rtc_region_valid() fails and every key
    // reads back as absent).
    memset(&s_region, 0, sizeof(s_region));
    return BB_OK;
}

static bool rtc_exists(void *impl, const bb_storage_addr_t *addr)
{
    (void)impl;

    if (addr->key == NULL) {
        return false;
    }

    if (!bb_storage_rtc_region_valid(&s_region)) {
        return false;
    }

    if (strcmp(addr->key, "ssid") == 0) {
        return s_region.ssid[0] != '\0';
    }
    if (strcmp(addr->key, "pass") == 0) {
        return s_region.pass[0] != '\0';
    }
    if (strcmp(addr->key, "provisioned") == 0) {
        // Region is already known-valid at this point -- "provisioned"
        // exists whenever the region does, regardless of the stored value.
        return true;
    }

    return false;
}

static const bb_storage_vtable_t s_rtc_vtable = {
    .get    = rtc_get,
    .set    = rtc_set,
    .erase  = rtc_erase,
    .exists = rtc_exists,
};

bb_err_t bb_storage_rtc_register(void)
{
    return bb_storage_register_backend("rtc", &s_rtc_vtable, NULL);
}
