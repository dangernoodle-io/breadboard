#pragma once

/**
 * @brief Foundational, near-zero-dep primitives every bb_* component builds
 * on: the portable error type, the canonical clock, run-exactly-once, a
 * contention-instrumented lock, byte-order helpers, memory accounting, and
 * the reboot-reason codec.
 */

// Foundational workspace types: error codes + opaque HTTP handles.
// Every bb_* component REQUIRES bb_core for these types.

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Error type and codes
// ---------------------------------------------------------------------------
// bb_err_t is a typedef for esp_err_t under ESP-IDF and int elsewhere —
// values pass through unchanged. The constants below alias to ESP_OK /
// ESP_ERR_* under ESP-IDF so esp_* return values compare correctly without
// translation at component boundaries.
//
// BB_ERR_NO_SPACE: bounded buffer or registry is full (capacity exhaustion).
// BB_ERR_NO_MEM: heap/memory allocation failed (malloc/OOM condition).
#ifdef ESP_PLATFORM
#include "esp_err.h"
typedef esp_err_t bb_err_t;
#define BB_OK                   ESP_OK
#define BB_ERR_INVALID_ARG      ESP_ERR_INVALID_ARG
#define BB_ERR_NOT_FOUND        ESP_ERR_NOT_FOUND
#define BB_ERR_NOT_INITIALIZED  ESP_ERR_NVS_NOT_INITIALIZED
#define BB_ERR_NO_SPACE         ESP_ERR_NO_MEM
#define BB_ERR_NO_MEM           ESP_ERR_NO_MEM
#define BB_ERR_INVALID_STATE    ESP_ERR_INVALID_STATE
#define BB_ERR_UNSUPPORTED      ESP_ERR_NOT_SUPPORTED
#define BB_ERR_TIMEOUT          ESP_ERR_TIMEOUT
// BB_ERR_BASE: breadboard-custom codes with no ESP-IDF component behind them
// (no matching esp_err_to_name() entry). Every ESP-IDF component range is
// documented and stays below 0x10000 (NVS 0x1100, ULP 0x1200, OTA 0x1500,
// WIFI 0x3000, MESH 0x4000, FLASH 0x6000, HW_CRYPTO 0xc000, MEMPROT 0xd000)
// — 0x10000 is chosen to sit clear of all of them so a bb_* code can never
// numerically alias a real ESP_ERR_* value. Previously these were encoded as
// ESP_ERR_INVALID_ARG + 0x1000/0x1001/0x1002, which landed inside NVS's
// reserved 0x1100+ range (BB_ERR_VALIDATION == ESP_ERR_NVS_NOT_FOUND,
// BB_ERR_CONFLICT == ESP_ERR_NVS_TYPE_MISMATCH) — do not reintroduce a base
// inside any documented ESP-IDF component range above.
#define BB_ERR_BASE             0x10000
#define BB_ERR_VALIDATION       (BB_ERR_BASE + 1)
#define BB_ERR_CONFLICT         (BB_ERR_BASE + 2)
#define BB_ERR_UNAUTHORIZED     (BB_ERR_BASE + 3)
#else
typedef int bb_err_t;
#define BB_OK                   0
#define BB_ERR_INVALID_ARG      1
#define BB_ERR_NOT_FOUND        2
#define BB_ERR_NOT_INITIALIZED  3
#define BB_ERR_NO_SPACE         4
#define BB_ERR_INVALID_STATE    5
#define BB_ERR_UNSUPPORTED      6
#define BB_ERR_VALIDATION       7
#define BB_ERR_TIMEOUT          8
#define BB_ERR_CONFLICT         9
#define BB_ERR_NO_MEM           10
#define BB_ERR_UNAUTHORIZED     11
#endif

// ---------------------------------------------------------------------------
// Opaque HTTP handles
// ---------------------------------------------------------------------------
// bb_http_handle_t identifies the HTTP server instance; bb_http_request_t
// identifies an in-flight request. Both are typedef'd to void * so that
// non-route components (e.g. bb_mdns implementing a registry init_fn that
// ignores the server arg) can use the type without REQUIRES bb_http.

typedef void *bb_http_handle_t;
typedef void *bb_http_request_t;

// ---------------------------------------------------------------------------
// Pause/resume callback types (shared by bb_ota_pull, bb_ota_check, etc.)
// ---------------------------------------------------------------------------
// pause_cb: called before an outbound HTTP fetch; return true to allow,
//           false to skip the fetch (resume will NOT be called on false).
// resume_cb: called after the fetch completes, on both success and failure.
typedef bool (*bb_http_pause_cb_t)(void);
typedef void (*bb_http_resume_cb_t)(void);

// ---------------------------------------------------------------------------
// OTA progress callback (shared by bb_ota_pull, bb_ota_push, bb_ota_boot)
// ---------------------------------------------------------------------------
// Lets a consumer surface OTA activity uniformly across every update path —
// e.g. flash an LED. Fired at phase transitions; `pct` (0..100) is meaningful
// only on BB_OTA_PHASE_PROGRESS (ignore it otherwise). Called from the worker
// task that performs the update — keep handlers short and non-blocking.
typedef enum {
    BB_OTA_PHASE_START,     // download/transfer beginning
    BB_OTA_PHASE_PROGRESS,  // in progress; pct = percent complete
    BB_OTA_PHASE_SUCCESS,   // image written/validated (reboot imminent)
    BB_OTA_PHASE_FAIL,      // aborted/failed
} bb_ota_phase_t;
typedef void (*bb_ota_progress_cb_t)(bb_ota_phase_t phase, int pct);

// ---------------------------------------------------------------------------
// OTA skip-check callback (shared by bb_ota_pull, bb_ota_push, bb_ota_hooks)
// ---------------------------------------------------------------------------
// Called before the project-name board mismatch check. Return true to skip
// the check and proceed with the OTA regardless, false to enforce it.
typedef bool (*bb_ota_skip_check_cb_t)(void);

#ifdef __cplusplus
}
#endif
