#include "bb_storage_ram.h"
#include "bb_storage.h"
#include "bb_str.h"
#include "bb_log.h"

#include <stddef.h>
#include <string.h>
#include <pthread.h>

static const char *TAG = "bb_storage_ram";

/* ---------------------------------------------------------------------------
 * Internal entry — fixed-capacity, no heap
 * ---------------------------------------------------------------------------*/
typedef struct {
    bool   used;
    char   key[BB_STORAGE_RAM_MAX_KEY_BYTES];
    size_t len;
    uint8_t value[BB_STORAGE_RAM_MAX_VALUE_BYTES];
} bb_storage_ram_entry_t;

static bb_storage_ram_entry_t s_entries[BB_STORAGE_RAM_MAX_ENTRIES];

// Guards s_entries against concurrent get/set/erase/exists from device tasks.
// Mutex approach: pthread_mutex_t on host; on ESP-IDF pthread.h is also
// available (ESP-IDF ships a POSIX pthread layer), so a single
// PTHREAD_MUTEX_INITIALIZER works on both targets — mirrors bb_power/bb_fan.
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;

void bb_storage_ram_test_reset(void)
{
    pthread_mutex_lock(&s_lock);
    memset(s_entries, 0, sizeof(s_entries));
    pthread_mutex_unlock(&s_lock);
}

// Callers must hold s_lock.
static bb_storage_ram_entry_t *find_entry(const char *key)
{
    if (key == NULL) {
        return NULL;
    }

    for (size_t i = 0; i < BB_STORAGE_RAM_MAX_ENTRIES; i++) {
        if (!s_entries[i].used) continue;
        if (strcmp(s_entries[i].key, key) == 0) {
            return &s_entries[i];
        }
    }

    return NULL;
}

// Callers must hold s_lock.
static bb_storage_ram_entry_t *find_free_slot(void)
{
    for (size_t i = 0; i < BB_STORAGE_RAM_MAX_ENTRIES; i++) {
        if (!s_entries[i].used) {
            return &s_entries[i];
        }
    }
    return NULL;
}

static bb_err_t ram_get(void *impl, const bb_storage_addr_t *addr, void *buf, size_t cap, size_t *out_len)
{
    (void)impl;

    pthread_mutex_lock(&s_lock);

    bb_storage_ram_entry_t *entry = find_entry(addr->key);
    if (entry == NULL) {
        pthread_mutex_unlock(&s_lock);
        return BB_ERR_NOT_FOUND;
    }

    *out_len = entry->len;
    if (cap > 0) {
        size_t copy_len = entry->len < cap ? entry->len : cap;
        memcpy(buf, entry->value, copy_len);
    }

    pthread_mutex_unlock(&s_lock);
    return BB_OK;
}

static bb_err_t ram_set(void *impl, const bb_storage_addr_t *addr, const void *buf, size_t len)
{
    (void)impl;

    if (addr->key == NULL || strlen(addr->key) >= BB_STORAGE_RAM_MAX_KEY_BYTES) {
        return BB_ERR_INVALID_ARG;
    }

    if (len > BB_STORAGE_RAM_MAX_VALUE_BYTES) {
        bb_log_w(TAG, "value for key '%s' (%u bytes) exceeds max %u bytes",
                 addr->key, (unsigned)len, (unsigned)BB_STORAGE_RAM_MAX_VALUE_BYTES);
        return BB_ERR_NO_SPACE;
    }

    pthread_mutex_lock(&s_lock);

    bb_storage_ram_entry_t *entry = find_entry(addr->key);
    if (entry == NULL) {
        entry = find_free_slot();
        if (entry == NULL) {
            pthread_mutex_unlock(&s_lock);
            bb_log_w(TAG, "table full (%u entries); cannot store key '%s'",
                     (unsigned)BB_STORAGE_RAM_MAX_ENTRIES, addr->key);
            return BB_ERR_NO_SPACE;
        }
        bb_strlcpy(entry->key, addr->key, sizeof(entry->key));
        entry->used = true;
    }

    if (len > 0) {
        memcpy(entry->value, buf, len);
    }
    entry->len = len;

    pthread_mutex_unlock(&s_lock);
    return BB_OK;
}

static bb_err_t ram_erase(void *impl, const bb_storage_addr_t *addr)
{
    (void)impl;

    pthread_mutex_lock(&s_lock);
    bb_storage_ram_entry_t *entry = find_entry(addr->key);
    if (entry != NULL) {
        memset(entry, 0, sizeof(*entry));
    }
    pthread_mutex_unlock(&s_lock);

    return BB_OK;
}

static bool ram_exists(void *impl, const bb_storage_addr_t *addr)
{
    (void)impl;

    pthread_mutex_lock(&s_lock);
    bool found = find_entry(addr->key) != NULL;
    pthread_mutex_unlock(&s_lock);

    return found;
}

static const bb_storage_vtable_t s_ram_vtable = {
    .get    = ram_get,
    .set    = ram_set,
    .erase  = ram_erase,
    .exists = ram_exists,
};

bb_err_t bb_storage_ram_register(void)
{
    return bb_storage_register_backend("ram", &s_ram_vtable, NULL);
}
