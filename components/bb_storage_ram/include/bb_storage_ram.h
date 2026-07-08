#pragma once

// bb_storage_ram — in-memory bb_storage backend (reference implementation).
//
// A fixed-capacity key -> value table, bounded by BB_STORAGE_RAM_MAX_ENTRIES
// entries of up to BB_STORAGE_RAM_MAX_VALUE_BYTES each. No heap allocation,
// no dynamic growth. Registered as backend "ram" via
// bb_storage_ram_register() — composition-only, no self-registration.
//
// Address field usage (see bb_storage.h): addr->ns_or_dir is ignored;
// addr->key is the map key (matched as a NUL-terminated C string). A key
// whose length (including the NUL terminator) is >= BB_STORAGE_RAM_MAX_KEY_BYTES
// is rejected outright with BB_ERR_INVALID_ARG -- never truncated -- see
// bb_storage_ram_set() in bb_storage_ram.c.
//
// Overflow behavior: bb_storage_set() on a new key once the table is full
// returns BB_ERR_NO_SPACE. A value longer than
// BB_STORAGE_RAM_MAX_VALUE_BYTES is rejected with BB_ERR_NO_SPACE (never
// silently truncated).

#include "bb_core.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Capacity constants (Kconfig bridge — pattern from bb_clock.h)
// ---------------------------------------------------------------------------
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#ifdef CONFIG_BB_STORAGE_RAM_MAX_ENTRIES
#define BB_STORAGE_RAM_MAX_ENTRIES CONFIG_BB_STORAGE_RAM_MAX_ENTRIES
#endif
#ifdef CONFIG_BB_STORAGE_RAM_MAX_VALUE_BYTES
#define BB_STORAGE_RAM_MAX_VALUE_BYTES CONFIG_BB_STORAGE_RAM_MAX_VALUE_BYTES
#endif
#ifdef CONFIG_BB_STORAGE_RAM_MAX_KEY_BYTES
#define BB_STORAGE_RAM_MAX_KEY_BYTES CONFIG_BB_STORAGE_RAM_MAX_KEY_BYTES
#endif
#endif
#ifndef BB_STORAGE_RAM_MAX_ENTRIES
#define BB_STORAGE_RAM_MAX_ENTRIES 32
#endif
#ifndef BB_STORAGE_RAM_MAX_VALUE_BYTES
#define BB_STORAGE_RAM_MAX_VALUE_BYTES 256
#endif
#ifndef BB_STORAGE_RAM_MAX_KEY_BYTES
#define BB_STORAGE_RAM_MAX_KEY_BYTES 64
#endif

// Clear the in-memory table (test/re-init use only).
void bb_storage_ram_test_reset(void);

// Register this backend with bb_storage under the name "ram". Idempotent
// registration policy is bb_storage's (first registration wins; a second
// call from the same process returns BB_ERR_INVALID_STATE, logged and
// harmless).
bb_err_t bb_storage_ram_register(void);

#ifdef __cplusplus
}
#endif
