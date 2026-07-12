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

/* ---------------------------------------------------------------------------
 * Multi-key transactions — buffering model (bb_storage.h's txn contract).
 *
 * Unlike bb_storage_nvs (which stages writes directly into a native,
 * already-open backing handle), this backend has no native "open handle,
 * write, commit" primitive — get/set above operate on the single static
 * s_region directly, one field at a time. So the txn group buffers each
 * staged key/value into txn->_slots[] (never touching s_region) and applies
 * every slot in one shot at commit(), making the whole batch crash-atomic:
 * a crash/abort between begin() and commit() leaves s_region exactly as it
 * was before the txn started — never a torn mix of old and new fields.
 * ---------------------------------------------------------------------------*/

// Bounds a staged value against its field's fixed capacity using the SAME
// sentinels rtc_set (above) enforces -- keeps the txn path's overflow
// behavior identical to the single-key path (BB_ERR_NO_SPACE, never a
// silent truncation). Also rejects an unrecognized key (BB_ERR_INVALID_ARG),
// same as rtc_set/rtc_erase.
static bb_err_t rtc_txn_classify(const char *key, size_t len)
{
    if (key == NULL) {
        return BB_ERR_INVALID_ARG;
    }
    if (strcmp(key, "ssid") == 0) {
        return (len > sizeof(s_region.ssid) - 1) ? BB_ERR_NO_SPACE : BB_OK;
    }
    if (strcmp(key, "pass") == 0) {
        return (len > sizeof(s_region.pass) - 1) ? BB_ERR_NO_SPACE : BB_OK;
    }
    if (strcmp(key, "provisioned") == 0) {
        return (len != sizeof(s_region.provisioned)) ? BB_ERR_INVALID_ARG : BB_OK;
    }
    return BB_ERR_INVALID_ARG;
}

static bb_err_t rtc_txn_begin(void *impl, bb_storage_txn_t *txn, const char *ns_or_dir)
{
    (void)impl;
    (void)ns_or_dir;  // ignored -- matches rtc_set/rtc_get, which ignore addr->ns_or_dir too.

    txn->_open = 1;
    txn->_err  = BB_OK;
    return BB_OK;
}

static bb_err_t rtc_txn_set(void *impl, bb_storage_txn_t *txn, const char *key, bb_storage_enc_t enc,
                             const void *buf, size_t len)
{
    (void)impl;

    if (!txn->_open) {
        return BB_ERR_INVALID_STATE;
    }
    if (txn->_err != BB_OK) {
        return txn->_err;
    }

    bb_err_t err = rtc_txn_classify(key, len);
    if (err != BB_OK) {
        txn->_err = err;
        return err;
    }

    // Key already validated/classified above (fixed ssid/pass/provisioned
    // set) -- stage into txn->_slots[] via the shared bb_storage helper
    // (same idiom bb_storage_ram's txn_set uses, factored out to
    // bb_storage_txn_slot_stage per B1-763).
    err = bb_storage_txn_slot_stage(txn, key, enc, buf, len);
    if (err != BB_OK) {
        txn->_err = err;
    }
    return err;
}

static bb_err_t rtc_txn_commit(void *impl, bb_storage_txn_t *txn)
{
    (void)impl;

    if (!txn->_open) {
        return BB_ERR_INVALID_STATE;
    }

    if (txn->_err != BB_OK) {
        // Poisoned -- close without publishing anything to s_region.
        txn->_open = 0;
        return txn->_err;
    }

    // Base the published region on the current live region IF it's already
    // valid (a partial txn -- e.g. ssid+pass staged, not provisioned -- must
    // preserve the third field's prior value), else start from an all-zero
    // base -- mirrors rtc_set's own invalid-region-promotes-to-zero rule.
    bb_storage_rtc_region_t tmp;
    if (bb_storage_rtc_region_valid(&s_region)) {
        tmp = s_region;
    } else {
        memset(&tmp, 0, sizeof(tmp));
    }

    for (int i = 0; i < BB_STORAGE_TXN_MAX_KEYS; i++) {
        if (!txn->_slots[i].used) {
            continue;
        }
        const char *key = txn->_slots[i].key;
        size_t      len = txn->_slots[i].len;
        const uint8_t *value = txn->_slots[i].value;

        if (strcmp(key, "ssid") == 0) {
            if (len > 0) {
                memcpy(tmp.ssid, value, len);
            }
            tmp.ssid[len] = '\0';
        } else if (strcmp(key, "pass") == 0) {
            if (len > 0) {
                memcpy(tmp.pass, value, len);
            }
            tmp.pass[len] = '\0';
        } else if (strcmp(key, "provisioned") == 0) {
            memcpy(&tmp.provisioned, value, sizeof(tmp.provisioned));
        }
    }

    tmp.magic   = BB_STORAGE_RTC_REGION_MAGIC;
    tmp.version = BB_STORAGE_RTC_REGION_VERSION;
    tmp.crc     = bb_storage_rtc_region_crc(&tmp);

    // Single struct assignment -- the ONLY line that touches the live
    // region, so a crash/reset can never observe a partially-applied txn:
    // s_region is either the pre-commit value (crash before this line) or
    // the fully-applied tmp (crash after), never anything in between.
    s_region = tmp;

    txn->_open = 0;
    return BB_OK;
}

static bb_err_t rtc_txn_abort(void *impl, bb_storage_txn_t *txn)
{
    (void)impl;

    if (!txn->_open) {
        return BB_OK;
    }

    // Discard staged slots -- s_region was never touched by set(), so
    // nothing to undo there.
    memset(txn->_slots, 0, sizeof(txn->_slots));
    txn->_open = 0;
    return BB_OK;
}

#ifdef BB_STORAGE_RTC_TESTING
bb_err_t bb_storage_rtc_txn_begin_for_test(bb_storage_txn_t *txn, const char *ns_or_dir)
{
    return rtc_txn_begin(NULL, txn, ns_or_dir);
}

bb_err_t bb_storage_rtc_txn_set_for_test(bb_storage_txn_t *txn, const char *key, bb_storage_enc_t enc,
                                          const void *buf, size_t len)
{
    return rtc_txn_set(NULL, txn, key, enc, buf, len);
}

bb_err_t bb_storage_rtc_txn_commit_for_test(bb_storage_txn_t *txn)
{
    return rtc_txn_commit(NULL, txn);
}

bb_err_t bb_storage_rtc_txn_abort_for_test(bb_storage_txn_t *txn)
{
    return rtc_txn_abort(NULL, txn);
}
#endif

static const bb_storage_vtable_t s_rtc_vtable = {
    .get        = rtc_get,
    .set        = rtc_set,
    .erase      = rtc_erase,
    .exists     = rtc_exists,
    .txn_begin  = rtc_txn_begin,
    .txn_set    = rtc_txn_set,
    .txn_commit = rtc_txn_commit,
    .txn_abort  = rtc_txn_abort,
};

bb_err_t bb_storage_rtc_register(void)
{
    return bb_storage_register_backend("rtc", &s_rtc_vtable, NULL);
}
