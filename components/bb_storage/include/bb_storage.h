#pragma once

// bb_storage — portable storage facade + backend registry.
//
// The single entry point for key/value-shaped persistence across
// heterogeneous backends (NVS, SD card, in-RAM, ...). A backend registers
// itself via bb_storage_register_backend(name, vtable, impl); every
// bb_storage_get/set/erase/exists call resolves addr->backend against the
// registry and dispatches to that backend's vtable. Unknown backend names
// return BB_ERR_NOT_FOUND — never a crash.
//
// bb_storage_addr_t is a flat tagged struct (chosen over a union for
// static-table ergonomics — designated-initializer literal tables read the
// same regardless of which backend a row targets). Field meaning is
// backend-specific:
//   backend == "nvs"    : ns_or_dir = NVS namespace (<=15 chars), key = NVS key
//   backend == "sdcard" : ns_or_dir = directory, key = filename
//   backend == "ram"    : ns_or_dir ignored/optional, key = the map key
//
// No self-registration: this component and every backend are
// composition-only. A backend is wired up by an explicit
// bb_storage_register_backend() call from application composition code or
// test setup — never a constructor, never an AUTOREGISTER Kconfig.

#include <stdbool.h>
#include <stddef.h>

#include "bb_core.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Capacity constants (Kconfig bridge — pattern from bb_clock.h)
// ---------------------------------------------------------------------------
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#ifdef CONFIG_BB_STORAGE_MAX_BACKENDS
#define BB_STORAGE_MAX_BACKENDS CONFIG_BB_STORAGE_MAX_BACKENDS
#endif
#endif
#ifndef BB_STORAGE_MAX_BACKENDS
#define BB_STORAGE_MAX_BACKENDS 4
#endif

// Address of a stored value, resolved against a registered backend by
// addr->backend. See the field-meaning table in the file header comment.
typedef struct {
    const char *backend;    // registry key, e.g. "nvs" | "sdcard" | "ram"
    const char *ns_or_dir;  // nvs: namespace; sdcard: dir; ram: ignored/optional
    const char *key;        // nvs: key; sdcard: filename; ram: key
} bb_storage_addr_t;

// Backend implementation. All four members must be non-NULL when passed to
// bb_storage_register_backend() — a partial vtable is rejected outright
// rather than crashing on a NULL call later.
typedef struct {
    bb_err_t (*get)(void *impl, const bb_storage_addr_t *addr, void *buf, size_t cap, size_t *out_len);
    bb_err_t (*set)(void *impl, const bb_storage_addr_t *addr, const void *buf, size_t len);
    bb_err_t (*erase)(void *impl, const bb_storage_addr_t *addr);
    bool     (*exists)(void *impl, const bb_storage_addr_t *addr);
} bb_storage_vtable_t;

// Clear the backend registry (test/re-init use only).
void bb_storage_test_reset(void);

// Register a backend under `name`. `name` must have static/registry-lifetime
// storage duration — the registry stores the raw pointer, not a copy (safe
// for the intended string-literal-at-init usage). `vt` is copied by value.
// Returns:
//   BB_OK                 on success
//   BB_ERR_INVALID_ARG    name, vt, or any vt member is NULL
//   BB_ERR_NO_SPACE        registry is full (BB_STORAGE_MAX_BACKENDS)
//   BB_ERR_INVALID_STATE   name already registered (first registration
//                          wins; the duplicate is logged and dropped)
bb_err_t bb_storage_register_backend(const char *name, const bb_storage_vtable_t *vt, void *impl);

// Read the value at addr into buf (capacity cap), writing the actual stored
// length to *out_len regardless of whether it fit in cap (mirrors
// snprintf-style "would have written" semantics) — callers detect
// truncation via (*out_len > cap). A size-probe (cap=0) and any read with
// cap >= the true length always succeed (BB_OK). A TRUNCATING read
// (0 < cap < true length) copies min(cap, len) bytes into buf and returns
// BB_OK on backends that can partial-read the stored value (e.g. ram); a
// backend that cannot partial-read (e.g. nvs, which must stage the full
// value before copying) MAY instead impose a bounded limit on the true
// length it will service for a truncating read and return
// BB_ERR_NO_SPACE beyond that limit — *out_len is still set to the true
// length in that case so the caller can retry with an adequately sized
// buffer.
// Returns:
//   BB_OK                 read succeeded (see truncation note above)
//   BB_ERR_INVALID_ARG    addr, addr->backend, buf (when cap>0), or
//                         out_len is NULL
//   BB_ERR_NOT_FOUND      no backend registered for addr->backend, or no
//                         value stored at addr
//   BB_ERR_NO_SPACE       backend cannot service this truncating read (see
//                         above); *out_len still reports the true length
bb_err_t bb_storage_get(const bb_storage_addr_t *addr, void *buf, size_t cap, size_t *out_len);

// Write len bytes from buf to addr, creating or overwriting the value.
// Returns:
//   BB_OK                 write succeeded
//   BB_ERR_INVALID_ARG    addr, addr->backend, or buf (when len>0) is NULL
//   BB_ERR_NOT_FOUND      no backend registered for addr->backend
//   BB_ERR_NO_SPACE        backend-specific capacity exhausted
bb_err_t bb_storage_set(const bb_storage_addr_t *addr, const void *buf, size_t len);

// Erase the value at addr, if any.
// Returns:
//   BB_OK                 erased (or already absent — erase is idempotent)
//   BB_ERR_INVALID_ARG    addr or addr->backend is NULL
//   BB_ERR_NOT_FOUND      no backend registered for addr->backend
bb_err_t bb_storage_erase(const bb_storage_addr_t *addr);

// Returns true iff a value is currently stored at addr. false for a NULL
// addr, a NULL addr->backend, or an unknown backend name — never a crash.
bool bb_storage_exists(const bb_storage_addr_t *addr);

#ifdef __cplusplus
}
#endif
