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
#include <stdint.h>

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
#ifdef CONFIG_BB_STORAGE_TXN_MAX_KEYS
#define BB_STORAGE_TXN_MAX_KEYS CONFIG_BB_STORAGE_TXN_MAX_KEYS
#endif
#ifdef CONFIG_BB_STORAGE_TXN_VALUE_MAX_BYTES
#define BB_STORAGE_TXN_VALUE_MAX_BYTES CONFIG_BB_STORAGE_TXN_VALUE_MAX_BYTES
#endif
#endif
#ifndef BB_STORAGE_MAX_BACKENDS
#define BB_STORAGE_MAX_BACKENDS 4
#endif
#ifndef BB_STORAGE_TXN_MAX_KEYS
#define BB_STORAGE_TXN_MAX_KEYS 3
#endif
#ifndef BB_STORAGE_TXN_VALUE_MAX_BYTES
#define BB_STORAGE_TXN_VALUE_MAX_BYTES 64
#endif
// Fixed — generic facade/RAM key ceiling, no Kconfig knob. NOT the NVS key
// limit: ESP-IDF's real NVS key-name limit is 15 chars + NUL
// (NVS_KEY_NAME_MAX_SIZE=16); the NVS backend enforces that stricter limit
// itself (see bb_storage_nvs.c's nvs_txn_set key-length guard).
#define BB_STORAGE_TXN_KEY_MAX_BYTES 32

// Address of a stored value, resolved against a registered backend by
// addr->backend. See the field-meaning table in the file header comment.
typedef struct {
    const char *backend;    // registry key, e.g. "nvs" | "sdcard" | "ram"
    const char *ns_or_dir;  // nvs: namespace; sdcard: dir; ram: ignored/optional
    const char *key;        // nvs: key; sdcard: filename; ram: key
} bb_storage_addr_t;

// Value encoding hint for bb_storage_get_typed/set_typed. BLOB is the
// default and is byte-identical to the plain get/set path above; the
// scalar/STR encodings let a backend that supports native typed entries
// (e.g. NVS's nvs_set_u8/u16/u32/i32/str) preserve on-flash type tags
// instead of writing everything as an untyped blob.
typedef enum {
    BB_STORAGE_ENC_BLOB = 0,
    BB_STORAGE_ENC_STR,
    BB_STORAGE_ENC_U8,
    BB_STORAGE_ENC_U16,
    BB_STORAGE_ENC_U32,
    BB_STORAGE_ENC_I32,
} bb_storage_enc_t;

// Forward declaration so the dispatch fn-ptr fields below and the vtable's
// txn_set/txn_commit/txn_abort members (declared further down, once this
// struct is complete) share ONE bb_storage_txn_t * signature. Without this,
// the fn-ptr fields would have to be typed `void *txn` (the struct's own
// type is incomplete inside its own definition) while the vtable members
// are typed `bb_storage_txn_t *txn`, bridged only by an explicit cast at
// each call site — a function-pointer type-pun that is undefined behavior
// per the C standard, even though every real call site happens to pass a
// bb_storage_txn_t *.
struct bb_storage_txn_s;
typedef struct bb_storage_txn_s bb_storage_txn_t;

// Opaque caller-allocated (stack/static, NO HEAP) multi-key transaction
// handle. One struct shape is shared across all backends; the dispatch
// fn-ptrs are captured at bb_storage_txn_begin() (reentrancy-safe — multiple
// concurrent txns are fine, each with its own handle). _handle is used by
// native-provisional backends (e.g. nvs, which stages writes in an
// nvs_handle_t and relies on commit/close for atomicity); _slots is used by
// buffering backends (e.g. ram, which stages key/value pairs and applies
// them atomically at commit). Both fields are always present so one struct
// serves all backends. Fields are backend-private — never inspect them
// outside a backend implementation.
//
// The caller MUST zero-initialize this struct (e.g. `bb_storage_txn_t txn =
// {0};`) before the first bb_storage_txn_begin() call on it — matching the
// existing bb_nv_batch_t caller-init convention (see bb_nv.h). This is not
// optional: bb_storage_txn_begin() reads txn->_open to reject a
// begin-on-already-open txn BEFORE it has zeroed anything, so a
// stack-garbage-initialized txn can spuriously fail its first begin() (or
// worse, look "open" and be silently skipped) depending on what garbage
// bits happen to be in that byte. Every helper in this file (begin/set/
// commit/abort) assumes the caller honored this contract.
typedef struct bb_storage_txn_s {
    bb_err_t (*_txn_set)(void *impl, bb_storage_txn_t *txn, const char *key, bb_storage_enc_t enc,
                          const void *buf, size_t len);
    bb_err_t (*_txn_commit)(void *impl, bb_storage_txn_t *txn);
    bb_err_t (*_txn_abort)(void *impl, bb_storage_txn_t *txn);
    void     *_impl;
    bb_err_t  _err;   // sticky first error, BB_OK initially
    uint8_t   _open;  // nonzero between begin and commit|abort
    uintptr_t _handle;      // backend-native handle (nvs_handle_t), 0 if unused
    struct {
        bool             used;
        char             key[BB_STORAGE_TXN_KEY_MAX_BYTES];
        uint8_t          value[BB_STORAGE_TXN_VALUE_MAX_BYTES];
        size_t           len;
        bb_storage_enc_t enc;
    } _slots[BB_STORAGE_TXN_MAX_KEYS];
} bb_storage_txn_t;

// Backend implementation. The first four members must be non-NULL when
// passed to bb_storage_register_backend() — a partial vtable is rejected
// outright rather than crashing on a NULL call later. get_typed/set_typed
// are OPTIONAL (nullable, as a pair): a backend that leaves them NULL gets
// blob semantics automatically via the facade's fallback to get/set (see
// bb_storage_get_typed/set_typed below) — RAM/host/sdcard backends need no
// code changes to keep their existing behavior.
//
// txn_begin/txn_set/txn_commit/txn_abort are an OPTIONAL group of four:
// all NULL or all set (validated at registration). No sequential fallback
// is provided (a NULL-group backend would silently violate atomicity) — a
// backend that leaves the group NULL makes bb_storage_txn_begin() return
// BB_ERR_UNSUPPORTED for that backend.
typedef struct {
    bb_err_t (*get)(void *impl, const bb_storage_addr_t *addr, void *buf, size_t cap, size_t *out_len);
    bb_err_t (*set)(void *impl, const bb_storage_addr_t *addr, const void *buf, size_t len);
    bb_err_t (*erase)(void *impl, const bb_storage_addr_t *addr);
    bool     (*exists)(void *impl, const bb_storage_addr_t *addr);

    // Optional (single member, not a pair/group) — NULL means the backend
    // does not support namespace-level erase. bb_storage_erase_namespace()
    // returns BB_ERR_UNSUPPORTED for that backend rather than silently
    // no-op'ing a destructive request: unlike get_typed/set_typed (which
    // fall back to blob semantics) there is no safe generic way to
    // enumerate and erase every key under ns_or_dir without a backend-native
    // "erase all" op, so no fallback is offered here either.
    bb_err_t (*erase_namespace)(void *impl, const char *ns_or_dir);

    // Optional (single member) — NULL means the backend does not support a
    // whole-PARTITION/whole-BACKEND erase. Fail-closed EXACTLY like
    // erase_namespace above: bb_storage_erase_all() returns
    // BB_ERR_UNSUPPORTED for a backend that leaves this NULL rather than
    // silently no-op'ing the most destructive request this facade exposes.
    // Broader than erase_namespace (which scopes to one ns_or_dir) — this
    // wipes every namespace/key the backend holds, e.g. the NVS backend's
    // whole-partition nvs_flash_erase(). No fallback is offered for the same
    // reason erase_namespace has none: there is no safe generic way to
    // enumerate and erase everything a backend holds without a
    // backend-native "erase all" op.
    bb_err_t (*erase_all)(void *impl);

    // Optional pair — both NULL or both set (validated at registration).
    bb_err_t (*get_typed)(void *impl, const bb_storage_addr_t *addr, bb_storage_enc_t enc,
                           void *buf, size_t cap, size_t *out_len);
    bb_err_t (*set_typed)(void *impl, const bb_storage_addr_t *addr, bb_storage_enc_t enc,
                           const void *buf, size_t len);

    // Optional group of four — all NULL or all set (validated at registration).
    bb_err_t (*txn_begin)(void *impl, bb_storage_txn_t *txn, const char *ns_or_dir);
    bb_err_t (*txn_set)(void *impl, bb_storage_txn_t *txn, const char *key, bb_storage_enc_t enc,
                         const void *buf, size_t len);
    bb_err_t (*txn_commit)(void *impl, bb_storage_txn_t *txn);  // ATOMIC + DURABLE
    bb_err_t (*txn_abort)(void *impl, bb_storage_txn_t *txn);
} bb_storage_vtable_t;

// Clear the backend registry (test/re-init use only).
void bb_storage_test_reset(void);

// Register a backend under `name`. `name` must have static/registry-lifetime
// storage duration — the registry stores the raw pointer, not a copy (safe
// for the intended string-literal-at-init usage). `vt` is copied by value.
// get_typed/set_typed are validated as a pair — one NULL and the other set
// is rejected the same as a missing mandatory member.
// Returns:
//   BB_OK                 on success
//   BB_ERR_INVALID_ARG    name, vt, any mandatory vt member is NULL, or
//                         get_typed/set_typed are not both NULL or both set
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

// Erase every key stored under `ns_or_dir` on `backend`. Idempotent — a
// namespace with nothing stored is not an error.
// Returns:
//   BB_OK                 erased (or already empty)
//   BB_ERR_INVALID_ARG    backend or ns_or_dir is NULL
//   BB_ERR_NOT_FOUND      no backend registered under `backend`
//   BB_ERR_UNSUPPORTED    the backend does not implement namespace-level
//                         erase (vtable erase_namespace is NULL) — never a
//                         silent no-op on a destructive request
bb_err_t bb_storage_erase_namespace(const char *backend, const char *ns_or_dir);

// Erase EVERYTHING `backend` holds (every namespace/key) — broader than
// bb_storage_erase_namespace, which scopes to one ns_or_dir. Idempotent.
// Returns:
//   BB_OK                 erased (or already empty)
//   BB_ERR_INVALID_ARG    backend is NULL
//   BB_ERR_NOT_FOUND      no backend registered under `backend`
//   BB_ERR_UNSUPPORTED    the backend does not implement whole-backend erase
//                         (vtable erase_all is NULL) — never a silent no-op
//                         on a destructive request
bb_err_t bb_storage_erase_all(const char *backend);

// Returns true iff a value is currently stored at addr. false for a NULL
// addr, a NULL addr->backend, or an unknown backend name — never a crash.
bool bb_storage_exists(const bb_storage_addr_t *addr);

// Type-aware read: dispatches to the backend's get_typed when the backend
// registered one, else falls back to plain bb_storage_get() (enc ignored —
// blob semantics, byte-identical to today's behavior). Same truncation/
// size-probe/error contract as bb_storage_get().
bb_err_t bb_storage_get_typed(const bb_storage_addr_t *addr, bb_storage_enc_t enc,
                               void *buf, size_t cap, size_t *out_len);

// Type-aware write: dispatches to the backend's set_typed when the backend
// registered one, else falls back to plain bb_storage_set() (enc ignored).
// Same error contract as bb_storage_set().
bb_err_t bb_storage_set_typed(const bb_storage_addr_t *addr, bb_storage_enc_t enc,
                               const void *buf, size_t len);

/* ---------------------------------------------------------------------------
 * Multi-key transactions
 *
 * Stage several key/value writes against one backend/namespace and commit
 * them as a single atomic + durable operation, or abort to discard them.
 * txn is caller-allocated (stack/static, no heap) — zero-initialize it
 * (e.g. `bb_storage_txn_t txn = {0};`) before the first bb_storage_txn_begin()
 * call; begin() checks the open flag to reject a begin-on-already-open txn,
 * so starting from garbage stack contents is unsafe. A txn is single-use:
 * commit and abort both close it; calling set() after either returns
 * BB_ERR_INVALID_STATE (abort is the one exception — idempotent/safe to call
 * again on an already-closed or never-opened *zero-initialized* txn).
 *
 *   bb_storage_txn_t txn;
 *   bb_err_t err = bb_storage_txn_begin("nvs", "wifi", &txn);
 *   if (err != BB_OK) return err;
 *   bb_storage_txn_set(&txn, "ssid", BB_STORAGE_ENC_STR, ssid, strlen(ssid));
 *   bb_storage_txn_set(&txn, "pass", BB_STORAGE_ENC_STR, pass, strlen(pass));
 *   err = bb_storage_txn_commit(&txn);   // atomic: both keys or neither
 *
 * Errors are sticky: the first txn_set failure poisons the txn and is
 * returned by bb_storage_txn_commit (which still closes the txn).
 * --------------------------------------------------------------------------- */

// Begin a multi-key transaction against `backend`/`ns_or_dir`. `txn` is
// zero-initialized and its dispatch fn-ptrs captured from the resolved
// backend entry.
// Returns:
//   BB_OK                 transaction opened
//   BB_ERR_INVALID_ARG    backend, ns_or_dir, or txn is NULL
//   BB_ERR_NOT_FOUND       no backend registered under `backend`
//   BB_ERR_UNSUPPORTED     the backend does not implement the txn group
bb_err_t bb_storage_txn_begin(const char *backend, const char *ns_or_dir, bb_storage_txn_t *txn);

// Stage a key/value write within an open transaction. Not applied until
// bb_storage_txn_commit().
// Returns:
//   BB_OK                 staged
//   BB_ERR_INVALID_ARG    txn or key is NULL, or key length is at/over
//                         BB_STORAGE_TXN_KEY_MAX_BYTES (the NVS backend
//                         additionally enforces its own stricter real NVS
//                         key-name limit, 15 chars + NUL)
//   BB_ERR_INVALID_STATE  txn is not open (never begun, or already
//                         committed/aborted)
//   BB_ERR_NO_SPACE        slot table full, or value exceeds
//                         BB_STORAGE_TXN_VALUE_MAX_BYTES
bb_err_t bb_storage_txn_set(bb_storage_txn_t *txn, const char *key, bb_storage_enc_t enc,
                             const void *buf, size_t len);

// Commit all staged writes atomically and durably, then close the
// transaction (single-use).
// Returns:
//   BB_OK                 all staged writes committed
//   BB_ERR_INVALID_ARG    txn is NULL
//   BB_ERR_INVALID_STATE  txn is not open (never begun, or already
//                         committed/aborted) — symmetric with
//                         bb_storage_txn_set's guard; unlike
//                         bb_storage_txn_abort below, a commit on a closed
//                         txn is a caller bug, not a silent no-op
//   (other)                the txn's sticky error (if a staged write already
//                         failed) or the backend's own commit error
bb_err_t bb_storage_txn_commit(bb_storage_txn_t *txn);

// Discard all staged writes and close the transaction. Idempotent/safe to
// call on a poisoned, already-closed, or never-begun txn.
// Returns BB_ERR_INVALID_ARG for a NULL txn, BB_OK otherwise.
bb_err_t bb_storage_txn_abort(bb_storage_txn_t *txn);

/* ---------------------------------------------------------------------------
 * Backend-internal helper — NOT part of the application-facing API above.
 *
 * bb_storage_txn_slot_stage() is the shared "buffer a (key, enc, value,
 * len) into txn->_slots[]" idiom used by buffering-model backends (e.g.
 * bb_storage_ram, bb_storage_rtc) inside their own txn_set vtable hook.
 * Declared here (not a separate header) because bb_storage owns
 * bb_storage_txn_t/_slots; a backend's own key validation/classification
 * (e.g. bb_storage_rtc's fixed ssid/pass/provisioned gate) is that
 * backend's concern and must run BEFORE calling this helper.
 * --------------------------------------------------------------------------- */

// Stage (key, enc, buf, len) into txn->_slots[]: reuses an already-staged
// slot for `key` (last-write-wins within the txn), else the first free
// slot. Bounds `len` against BB_STORAGE_TXN_VALUE_MAX_BYTES and `key`
// against BB_STORAGE_TXN_KEY_MAX_BYTES.
// Returns:
//   BB_OK                staged
//   BB_ERR_INVALID_ARG   txn or key is NULL, or key length is at/over
//                        BB_STORAGE_TXN_KEY_MAX_BYTES
//   BB_ERR_NO_SPACE       value exceeds BB_STORAGE_TXN_VALUE_MAX_BYTES, or
//                        the slot table is full
bb_err_t bb_storage_txn_slot_stage(bb_storage_txn_t *txn, const char *key, bb_storage_enc_t enc,
                                    const void *buf, size_t len);

#ifdef __cplusplus
}
#endif
