// ESP-IDF target selftest for KB 791 Option A (type-aware bb_storage/
// bb_config NVS entries). Proves byte-identity between bb_nv's direct typed
// NVS accessors and bb_config's facade-backed typed accessors: a value
// written via one is readable via the other with no TYPE_MISMATCH,
// because both ultimately call the same native nvs_get/set_str.
//
// Runs automatically at boot (called from entry_espidf.c after
// bb_app_init_early(), once NVS is ready) and logs PASS/FAIL — the smoke
// build compiling this file is the CI proof point; the on-device NVS
// round-trip itself is verified at the hardware flash gate.

#include "storage_typed_selftest.h"

#ifdef ESP_PLATFORM

#include <string.h>

#include "bb_log.h"
#include "bb_nv.h"
#include "bb_config.h"
#include "bb_storage.h"
#include "bb_storage_nvs.h"

static const char *TAG = "storage_typed_selftest";

#define SELFTEST_NS       "bb_cfg"
#define SELFTEST_KEY      "wifi_ssid"
// One-shot guard: this selftest erases + rewrites NVS twice per run, which
// is fine for a single flash-gate validation pass but would wear the flash
// if it ran on every boot forever. Run it once (gated on a persisted marker)
// rather than never running it at all -- the smoke build still compiles it
// unconditionally under -Werror so CI proves it builds.
#define SELFTEST_DONE_KEY "typed_st_done"

static const bb_config_field_t s_ssid_field = {
    .id      = "wifi.ssid",
    .type    = BB_CONFIG_STR,
    .addr    = { .backend = "nvs", .ns_or_dir = SELFTEST_NS, .key = SELFTEST_KEY },
    .max_len = 33,
};

void bb_smoke_storage_typed_selftest(void)
{
    // Run once (persisted marker), never on every boot -- this test does an
    // erase + two NVS writes, which would wear the flash if it repeated on
    // every boot of a long-lived board. On-device byte-format validation is
    // a one-time flash-gate check, not a per-boot regression test.
    if (bb_nv_exists(SELFTEST_NS, SELFTEST_DONE_KEY)) {
        bb_log_i(TAG, "SKIP: already ran (one-shot marker set)");
        return;
    }
    // Mark done up front (before the erase/write cycle below) so the
    // one-shot bound holds even on a failure path -- a persistently failing
    // selftest should be investigated at the flash gate, not retried every
    // boot for the life of the device.
    (void)bb_nv_set_u8(SELFTEST_NS, SELFTEST_DONE_KEY, 1);

    // bb_storage_nvs_register() is idempotent-by-policy (first-wins); a
    // repeat call across warm boots is expected and harmless.
    bb_err_t rc = bb_storage_nvs_register();
    if (rc != BB_OK && rc != BB_ERR_INVALID_STATE) {
        bb_log_e(TAG, "FAIL: bb_storage_nvs_register rc=%d", (int)rc);
        return;
    }

    (void)bb_storage_nvs_erase(SELFTEST_NS, SELFTEST_KEY);

    // (1) bb_nv_set_str (simulates a provisioned board) -> bb_config_get_str
    // at the same ns/key with enc=STR. No TYPE_MISMATCH, byte-identical.
    const char *seed = "MyNetwork";
    rc = bb_nv_set_str(SELFTEST_NS, SELFTEST_KEY, seed);
    if (rc != BB_OK) {
        bb_log_e(TAG, "FAIL: bb_nv_set_str rc=%d", (int)rc);
        return;
    }

    char buf[40] = {0};
    size_t out_len = 0;
    rc = bb_config_get_str(&s_ssid_field, buf, sizeof(buf), &out_len);
    if (rc != BB_OK) {
        bb_log_e(TAG, "FAIL: bb_config_get_str rc=%d (expected BB_OK, no TYPE_MISMATCH)", (int)rc);
        return;
    }
    if (out_len != strlen(seed) || strncmp(buf, seed, out_len) != 0) {
        bb_log_e(TAG, "FAIL: bb_config_get_str mismatch: got '%.*s' want '%s'",
                 (int)out_len, buf, seed);
        return;
    }

    // (2) reverse: bb_config_set_str -> bb_nv_get_str at the same ns/key.
    const char *updated = "OtherNetwork";
    rc = bb_config_set_str(&s_ssid_field, updated);
    if (rc != BB_OK) {
        bb_log_e(TAG, "FAIL: bb_config_set_str rc=%d", (int)rc);
        return;
    }

    char buf2[40] = {0};
    rc = bb_nv_get_str(SELFTEST_NS, SELFTEST_KEY, buf2, sizeof(buf2), NULL);
    if (rc != BB_OK) {
        bb_log_e(TAG, "FAIL: bb_nv_get_str rc=%d", (int)rc);
        return;
    }
    if (strcmp(buf2, updated) != 0) {
        bb_log_e(TAG, "FAIL: bb_nv_get_str mismatch: got '%s' want '%s'", buf2, updated);
        return;
    }

    // (3) HIGH finding regression (nvs_vt_get_typed_str NUL off-by-one):
    // PROBE for the true length, then retry with a buffer allocated to
    // EXACTLY that length (cap == str_len, no room for a NUL) -- the natural
    // caller pattern this bug broke. Must succeed (BOUNCE, not a raw
    // ESP_ERR_NVS_INVALID_LENGTH leaking through bb_config_get_str).
    size_t probe_len = 0;
    rc = bb_config_get_str(&s_ssid_field, NULL, 0, &probe_len);
    if (rc != BB_OK) {
        bb_log_e(TAG, "FAIL: bb_config_get_str probe rc=%d", (int)rc);
        return;
    }
    if (probe_len != strlen(updated)) {
        bb_log_e(TAG, "FAIL: bb_config_get_str probe len=%u want=%u", (unsigned)probe_len,
                 (unsigned)strlen(updated));
        return;
    }

    char exact_buf[64] = {0};
    size_t exact_len = 0;
    rc = bb_config_get_str(&s_ssid_field, exact_buf, probe_len, &exact_len);
    if (rc != BB_OK) {
        bb_log_e(TAG, "FAIL: bb_config_get_str cap==str_len rc=%d (expected BB_OK, not NO_SPACE)",
                 (int)rc);
        return;
    }
    if (exact_len != probe_len || strncmp(exact_buf, updated, exact_len) != 0) {
        bb_log_e(TAG, "FAIL: bb_config_get_str cap==str_len mismatch: got '%.*s' want '%s'",
                 (int)exact_len, exact_buf, updated);
        return;
    }

    bb_log_i(TAG, "PASS: bb_nv/bb_config STR byte-identity round trip (both directions)");
}

#else /* !ESP_PLATFORM */

void bb_smoke_storage_typed_selftest(void)
{
    // Host/Arduino: bb_storage_nvs is ESP-IDF-only (see its header) --
    // nothing to self-test off-target.
}

#endif
