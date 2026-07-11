#include "test_hostname_seed.h"
#include "bb_settings.h"
#include "bb_storage.h"

#include <stdbool.h>
#include <string.h>

// Minimal in-memory "nvs" backend, scoped to this helper only (mirrors
// test_bb_settings.c's fake_nvs_* pattern). Registered lazily/idempotently
// on first use per process, and self-heals if a prior test wiped the
// backend registry via bb_storage_test_reset().
#define SEED_NVS_MAX_ENTRIES 4
#define SEED_NVS_MAX_VALUE   64
#define SEED_NVS_KEY_MAX     16

typedef struct {
    bool   used;
    char   key[SEED_NVS_KEY_MAX];
    size_t len;
    uint8_t value[SEED_NVS_MAX_VALUE];
} seed_nvs_entry_t;

static seed_nvs_entry_t s_entries[SEED_NVS_MAX_ENTRIES];

static seed_nvs_entry_t *seed_find(const char *key)
{
    if (key == NULL) return NULL;
    for (int i = 0; i < SEED_NVS_MAX_ENTRIES; i++) {
        if (s_entries[i].used && strcmp(s_entries[i].key, key) == 0) {
            return &s_entries[i];
        }
    }
    return NULL;
}

static bb_err_t seed_get(void *impl, const bb_storage_addr_t *addr, void *buf, size_t cap, size_t *out_len)
{
    (void)impl;
    seed_nvs_entry_t *e = seed_find(addr->key);
    if (e == NULL) return BB_ERR_NOT_FOUND;
    *out_len = e->len;
    if (cap > 0) {
        size_t copy_len = e->len < cap ? e->len : cap;
        memcpy(buf, e->value, copy_len);
    }
    return BB_OK;
}

static bb_err_t seed_set(void *impl, const bb_storage_addr_t *addr, const void *buf, size_t len)
{
    (void)impl;
    if (len > SEED_NVS_MAX_VALUE) return BB_ERR_NO_SPACE;

    seed_nvs_entry_t *e = seed_find(addr->key);
    if (e == NULL) {
        for (int i = 0; i < SEED_NVS_MAX_ENTRIES; i++) {
            if (!s_entries[i].used) { e = &s_entries[i]; break; }
        }
        if (e == NULL) return BB_ERR_NO_SPACE;
        strncpy(e->key, addr->key, sizeof(e->key) - 1);
        e->key[sizeof(e->key) - 1] = '\0';
        e->used = true;
    }
    if (len > 0) memcpy(e->value, buf, len);
    e->len = len;
    return BB_OK;
}

static bb_err_t seed_erase(void *impl, const bb_storage_addr_t *addr)
{
    (void)impl;
    seed_nvs_entry_t *e = seed_find(addr->key);
    if (e != NULL) memset(e, 0, sizeof(*e));
    return BB_OK;
}

static bool seed_exists(void *impl, const bb_storage_addr_t *addr)
{
    (void)impl;
    return seed_find(addr->key) != NULL;
}

static const bb_storage_vtable_t s_seed_vtable = {
    .get    = seed_get,
    .set    = seed_set,
    .erase  = seed_erase,
    .exists = seed_exists,
};

void bb_test_seed_hostname(const char *hostname)
{
    // Duplicate registration (BB_ERR_INVALID_STATE) is expected and ignored
    // -- another test file, or an earlier call from this same helper, may
    // already own the "nvs" slot.
    bb_storage_register_backend("nvs", &s_seed_vtable, NULL);
    bb_settings_hostname_set(hostname);
}

void bb_test_seed_hostname_clear(void)
{
    bb_storage_register_backend("nvs", &s_seed_vtable, NULL);
    // Same addr triple bb_settings.c's s_hostname_field targets.
    static const bb_storage_addr_t addr = { .backend = "nvs", .ns_or_dir = "bb_cfg", .key = "hostname" };
    bb_storage_erase(&addr);
}
