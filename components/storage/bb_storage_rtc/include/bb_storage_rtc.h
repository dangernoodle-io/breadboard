#pragma once

// bb_storage_rtc — RTC-mirror bb_storage backend for WiFi credentials.
//
// @brief Warm-reboot RTC-mirror `bb_storage` backend for WiFi credentials.
//
// Implements the bb_storage_vtable_t contract (get/set/erase/exists) over a
// single bb_storage_rtc_region_t instance held in RTC "no-init" memory on
// ESP-IDF (a plain static on host, standing in for RTC_NOINIT so the same
// contract is testable off-device). Registered as backend "rtc" via
// bb_storage_rtc_register() — composition-only, no self-registration.
//
// This is a warm-reboot / crash-recovery cache, NOT durable storage: the
// region survives software reset, panic, and deep sleep, but is LOST on a
// full power cycle. NVS (bb_storage_nvs) stays the source of truth for
// credentials; this backend exists to restore them fast, without an NVS
// round-trip, immediately after a crash/watchdog reboot.
//
// Keyed model — three fixed keys against addr->backend=="rtc"
// (addr->ns_or_dir is ignored, mirroring "ram"):
//   "ssid"        - string, max 31 chars + NUL
//   "pass"        - string, max 63 chars + NUL
//   "provisioned" - uint8_t
// An unknown key returns BB_ERR_NOT_FOUND from get/exists and
// BB_ERR_INVALID_ARG from set.
//
// Whole-region magic+version+CRC guard: get() first validates the WHOLE
// region (bb_storage_rtc_region_valid()); an invalid region (cold boot,
// corruption, or a version mismatch from an older/newer image) makes EVERY
// key read BB_ERR_NOT_FOUND, regardless of stale bytes still sitting in RTC
// memory — "cold boot == empty" is the contract callers rely on. set()
// bounds-checks the incoming value against its field's fixed capacity
// (overflow -> BB_ERR_NO_SPACE, never silently truncated — SSID/pass are
// already protocol-capped upstream so this never triggers in practice),
// writes the one slot, then ALWAYS re-stamps magic/version and recomputes
// the whole-region CRC, so any single set() leaves a structurally valid,
// healable region. erase() invalidates the WHOLE region (zeroes it, same as
// today's bb_nv clear paths) after validating the key itself (an unknown key
// still returns BB_ERR_INVALID_ARG). exists() is true iff the region is
// valid AND the field is non-empty (str) / true (provisioned, i.e. the
// region is valid at all).
//
// PR3a (this component's introduction) is ADDITIVE ONLY: bb_storage_rtc is
// registered and composable, but bb_nv's own s_creds_mirror (a SEPARATE RTC
// no-init region) still owns the live write/heal duty end to end — the two
// regions coexist, deliberately, until PR3b relocates bb_nv's mirror
// read/write/heal call sites onto this backend and collapses to one region.

#include "bb_core.h"

#ifdef BB_STORAGE_RTC_TESTING
// bb_storage_rtc_region.h and bb_storage.h (bb_storage_txn_t,
// bb_storage_enc_t) are only needed by the testing-only prototypes below --
// kept out of the unconditional include list so a production (non-TESTING)
// build of this header never pulls in the region layout or bb_storage.h's
// public REQUIRES for every bb_storage_rtc.h consumer, mirroring
// bb_storage_nvs.h's BB_STORAGE_NVS_TESTING-gated bb_storage.h include.
#include "bb_storage_rtc_region.h"
#include "bb_storage.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Clear the region (test/re-init use only) — simulates a cold boot (RTC
// memory zeroed, e.g. after a full power cycle) on host builds where there
// is no real RTC_NOINIT memory to lose power. On-device this is test-only
// plumbing; production code never calls it.
void bb_storage_rtc_test_reset(void);

#ifdef BB_STORAGE_RTC_TESTING
// Direct pointer to the backend's static region — testing-only, so a host
// test can poke individual bytes (CRC bit-flip, version mismatch) to prove
// bb_storage_rtc_region_valid()'s corruption/version-mismatch guard makes
// every subsequent get() fail closed (BB_ERR_NOT_FOUND) rather than crash.
// Never call this from production code.
bb_storage_rtc_region_t *bb_storage_rtc_region_for_test(void);

// Thin wrappers around the backend's static txn_begin/set/commit/abort
// (normally only reachable through the registered bb_storage_vtable_t) --
// testing-only direct-drive seam, mirroring bb_storage_nvs's
// _txn_*_for_test pattern. Never call these from production code; a real
// consumer always goes through bb_storage_txn_begin("rtc", ...).
bb_err_t bb_storage_rtc_txn_begin_for_test(bb_storage_txn_t *txn, const char *ns_or_dir);
bb_err_t bb_storage_rtc_txn_set_for_test(bb_storage_txn_t *txn, const char *key, bb_storage_enc_t enc,
                                          const void *buf, size_t len);
bb_err_t bb_storage_rtc_txn_commit_for_test(bb_storage_txn_t *txn);
bb_err_t bb_storage_rtc_txn_abort_for_test(bb_storage_txn_t *txn);
#endif

// Register this backend with bb_storage under the name "rtc". Idempotent
// registration policy is bb_storage's (first registration wins; a second
// call from the same process returns BB_ERR_INVALID_STATE, logged and
// harmless).
//
// provides=storage_rtc: an INERT provider in PR3a — no component yet
// requires=storage_rtc (bb_nv keeps its own separate RTC region until
// PR3b). Composed here purely to prove the registration path links and
// runs cleanly on the boot path ahead of the relocation.
// bbtool:init tier=early fn=bb_storage_rtc_register provides=storage_rtc
bb_err_t bb_storage_rtc_register(void);

#ifdef __cplusplus
}
#endif
