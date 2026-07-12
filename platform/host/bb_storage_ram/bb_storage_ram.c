#include "bb_storage_ram.h"
#include "bb_storage.h"
#include "bb_str.h"
#include "bb_log.h"

#include <stddef.h>
#include <string.h>
#include <pthread.h>

static const char *TAG = "bb_storage_ram";

// Cross-Kconfig invariant: BB_STORAGE_RAM_MAX_VALUE_BYTES/KEY_BYTES
// (bb_storage_ram.h) and BB_STORAGE_TXN_VALUE_MAX_BYTES/KEY_MAX_BYTES
// (bb_storage.h) are independent Kconfig knobs. ram_txn_set bounds a staged
// value/key by the TXN cap; ram_txn_commit then memcpy's it into
// entry->value/key, sized by the RAM cap. If a deployment ever sets a RAM
// cap below the matching TXN cap, a legal txn_set (accepted because it is
// under the TXN cap) would overflow the static s_entries table on commit.
// Fail the build instead of BSS — this makes the illegal config
// uncompilable, zero runtime cost.
_Static_assert(BB_STORAGE_RAM_MAX_VALUE_BYTES >= BB_STORAGE_TXN_VALUE_MAX_BYTES,
               "BB_STORAGE_RAM_MAX_VALUE_BYTES must be >= BB_STORAGE_TXN_VALUE_MAX_BYTES "
               "(ram_txn_commit copies a txn-capped value into a RAM-capped slot)");
_Static_assert(BB_STORAGE_RAM_MAX_KEY_BYTES >= BB_STORAGE_TXN_KEY_MAX_BYTES,
               "BB_STORAGE_RAM_MAX_KEY_BYTES must be >= BB_STORAGE_TXN_KEY_MAX_BYTES "
               "(ram_txn_commit copies a txn-capped key into a RAM-capped slot)");

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

/* ---------------------------------------------------------------------------
 * Multi-key transactions — buffering backend. Writes are staged in
 * txn->_slots (txn-local, no lock needed) and applied atomically at commit:
 * one held-lock pass verifies capacity for every staged key before any write
 * lands, so a failing commit leaves the table completely untouched.
 * ---------------------------------------------------------------------------*/
static bb_err_t ram_txn_begin(void *impl, bb_storage_txn_t *txn, const char *ns_or_dir)
{
    (void)impl;
    (void)ns_or_dir;  // ram ignores namespace, same as the plain get/set path

    memset(txn->_slots, 0, sizeof(txn->_slots));
    txn->_open = 1;
    txn->_err  = BB_OK;
    return BB_OK;
}

static bb_err_t ram_txn_set(void *impl, bb_storage_txn_t *txn, const char *key, bb_storage_enc_t enc,
                             const void *buf, size_t len)
{
    (void)impl;

    // ram has no key gate of its own (arbitrary keys accepted) -- stage
    // directly via the shared bb_storage helper.
    return bb_storage_txn_slot_stage(txn, key, enc, buf, len);
}

static bb_err_t ram_txn_commit(void *impl, bb_storage_txn_t *txn)
{
    (void)impl;

    if (txn->_err != BB_OK) {
        txn->_open = 0;
        return txn->_err;
    }

    pthread_mutex_lock(&s_lock);

    // Pre-check: verify capacity for every staged key before any write
    // lands — a failing commit must leave s_entries completely untouched.
    size_t new_keys_needed = 0;
    for (size_t i = 0; i < BB_STORAGE_TXN_MAX_KEYS; i++) {
        if (!txn->_slots[i].used) continue;
        if (find_entry(txn->_slots[i].key) == NULL) {
            new_keys_needed++;
        }
    }

    size_t free_count = 0;
    for (size_t i = 0; i < BB_STORAGE_RAM_MAX_ENTRIES; i++) {
        if (!s_entries[i].used) free_count++;
    }

    if (new_keys_needed > free_count) {
        pthread_mutex_unlock(&s_lock);
        txn->_open = 0;
        return BB_ERR_NO_SPACE;
    }

    // Apply — same find_entry/find_free_slot logic ram_set uses, inlined
    // under the already-held lock (ram_set re-locks, so it cannot be called
    // here).
    for (size_t i = 0; i < BB_STORAGE_TXN_MAX_KEYS; i++) {
        if (!txn->_slots[i].used) continue;

        bb_storage_ram_entry_t *entry = find_entry(txn->_slots[i].key);
        if (entry == NULL) {
            entry = find_free_slot();
            bb_strlcpy(entry->key, txn->_slots[i].key, sizeof(entry->key));
            entry->used = true;
        }
        if (txn->_slots[i].len > 0) {
            memcpy(entry->value, txn->_slots[i].value, txn->_slots[i].len);
        }
        entry->len = txn->_slots[i].len;
    }

    pthread_mutex_unlock(&s_lock);
    txn->_open = 0;
    return BB_OK;
}

static bb_err_t ram_txn_abort(void *impl, bb_storage_txn_t *txn)
{
    (void)impl;

    // Slots are txn-local — nothing to unwind, no lock needed.
    txn->_open = 0;
    return BB_OK;
}

static const bb_storage_vtable_t s_ram_vtable = {
    .get        = ram_get,
    .set        = ram_set,
    .erase      = ram_erase,
    .exists     = ram_exists,
    .txn_begin  = ram_txn_begin,
    .txn_set    = ram_txn_set,
    .txn_commit = ram_txn_commit,
    .txn_abort  = ram_txn_abort,
};

bb_err_t bb_storage_ram_register(void)
{
    return bb_storage_register_backend("ram", &s_ram_vtable, NULL);
}
