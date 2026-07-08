#include "bb_storage.h"
#include "bb_log.h"

#include <stddef.h>
#include <string.h>

static const char *TAG = "bb_storage";

/* ---------------------------------------------------------------------------
 * Internal backend entry
 * ---------------------------------------------------------------------------*/
typedef struct {
    const char           *name;
    bb_storage_vtable_t   vt;
    void                 *impl;
} bb_storage_backend_entry_t;

/* ---------------------------------------------------------------------------
 * File-scope state — no heap, no ESP, s_ prefix per house rules
 *
 * Registration-time-only contract: backends are registered once at
 * composition/init (single writer, per the "composition-only" convention in
 * bb_storage.h) — bb_storage_register_backend() is NOT concurrency-safe
 * against itself or against a concurrent get/set/erase/exists dispatch.
 * Per-call get/set/erase/exists dispatch (below) is read-only against this
 * registry once composition has finished, so no runtime lock is added here;
 * any locking a backend needs for its own data is that backend's concern
 * (see bb_storage_ram's s_lock for the RAM backend's contract).
 * ---------------------------------------------------------------------------*/
static bb_storage_backend_entry_t s_backends[BB_STORAGE_MAX_BACKENDS];
static size_t                     s_count;

void bb_storage_test_reset(void)
{
    memset(s_backends, 0, sizeof(s_backends));
    s_count = 0;
}

bb_err_t bb_storage_register_backend(const char *name, const bb_storage_vtable_t *vt, void *impl)
{
    if (name == NULL || vt == NULL || vt->get == NULL || vt->set == NULL ||
        vt->erase == NULL || vt->exists == NULL) {
        return BB_ERR_INVALID_ARG;
    }

    for (size_t i = 0; i < s_count; i++) {
        if (strcmp(s_backends[i].name, name) == 0) {
            bb_log_w(TAG, "duplicate backend '%s' ignored (first registration wins)", name);
            return BB_ERR_INVALID_STATE;
        }
    }

    if (s_count >= BB_STORAGE_MAX_BACKENDS) {
        return BB_ERR_NO_SPACE;
    }

    s_backends[s_count].name = name;
    s_backends[s_count].vt   = *vt;
    s_backends[s_count].impl = impl;
    s_count++;

    return BB_OK;
}

static const bb_storage_backend_entry_t *find_backend(const char *name)
{
    if (name == NULL) {
        return NULL;
    }

    for (size_t i = 0; i < s_count; i++) {
        if (strcmp(s_backends[i].name, name) == 0) {
            return &s_backends[i];
        }
    }

    return NULL;
}

bb_err_t bb_storage_get(const bb_storage_addr_t *addr, void *buf, size_t cap, size_t *out_len)
{
    if (addr == NULL || addr->backend == NULL || out_len == NULL || (cap > 0 && buf == NULL)) {
        return BB_ERR_INVALID_ARG;
    }

    const bb_storage_backend_entry_t *entry = find_backend(addr->backend);
    if (entry == NULL) {
        return BB_ERR_NOT_FOUND;
    }

    return entry->vt.get(entry->impl, addr, buf, cap, out_len);
}

bb_err_t bb_storage_set(const bb_storage_addr_t *addr, const void *buf, size_t len)
{
    if (addr == NULL || addr->backend == NULL || (len > 0 && buf == NULL)) {
        return BB_ERR_INVALID_ARG;
    }

    const bb_storage_backend_entry_t *entry = find_backend(addr->backend);
    if (entry == NULL) {
        return BB_ERR_NOT_FOUND;
    }

    return entry->vt.set(entry->impl, addr, buf, len);
}

bb_err_t bb_storage_erase(const bb_storage_addr_t *addr)
{
    if (addr == NULL || addr->backend == NULL) {
        return BB_ERR_INVALID_ARG;
    }

    const bb_storage_backend_entry_t *entry = find_backend(addr->backend);
    if (entry == NULL) {
        return BB_ERR_NOT_FOUND;
    }

    return entry->vt.erase(entry->impl, addr);
}

bool bb_storage_exists(const bb_storage_addr_t *addr)
{
    if (addr == NULL || addr->backend == NULL) {
        return false;
    }

    const bb_storage_backend_entry_t *entry = find_backend(addr->backend);
    if (entry == NULL) {
        return false;
    }

    return entry->vt.exists(entry->impl, addr);
}
