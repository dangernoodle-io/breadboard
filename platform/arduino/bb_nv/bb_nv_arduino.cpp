#include <Arduino.h>
#include <EEPROM.h>
#include <string.h>
#include "bb_nv.h"

// EEPROM layout: magic (4B) | count (2B) | records...
// Each record: ns_len (1B) | ns | key_len (1B) | key | type (1B) | val_len (1B) | val
// type: 0=u8, 1=u32, 2=str, 0xFF=tombstone

#define MAGIC_OFFSET 0
#define MAGIC_VALUE  0x4231303000ULL  // "BB10\0" as bytes: 'B'=0x42, 'B'=0x42, '1'=0x31, '0'=0x30
#define COUNT_OFFSET 4
#define RECORD_START 6
#define EEPROM_SIZE  1024

static bool initialized = false;

extern "C" bb_err_t bb_nv_config_init(void) {
    // Check magic
    if (EEPROM.read(MAGIC_OFFSET) == 'B' &&
        EEPROM.read(MAGIC_OFFSET + 1) == 'B' &&
        EEPROM.read(MAGIC_OFFSET + 2) == '1' &&
        EEPROM.read(MAGIC_OFFSET + 3) == '0') {
        initialized = true;
        return BB_OK;
    }

    // Write header
    EEPROM.write(MAGIC_OFFSET, 'B');
    EEPROM.write(MAGIC_OFFSET + 1, 'B');
    EEPROM.write(MAGIC_OFFSET + 2, '1');
    EEPROM.write(MAGIC_OFFSET + 3, '0');
    EEPROM.write(COUNT_OFFSET, 0);
    EEPROM.write(COUNT_OFFSET + 1, 0);

    initialized = true;
    return BB_OK;
}

extern "C" bb_err_t bb_nv_flash_init(void) {
    // Arduino has no NVS partition concept; init is a no-op
    return BB_OK;
}

// Helper: scan EEPROM records to find (ns, key, type)
// Returns offset of value if found, -1 if not found, -2 if error
static int find_record(const char *ns, const char *key, uint8_t expected_type) {
    uint16_t count = (uint16_t)EEPROM.read(COUNT_OFFSET) | ((uint16_t)EEPROM.read(COUNT_OFFSET + 1) << 8);

    int offset = RECORD_START;
    for (uint16_t i = 0; i < count; i++) {
        if (offset + 5 > EEPROM_SIZE) return -2;  // Corrupted

        uint8_t ns_len = EEPROM.read(offset);
        offset++;

        if (offset + ns_len > EEPROM_SIZE) return -2;

        // Compare namespace
        bool ns_match = (strlen(ns) == ns_len);
        if (ns_match) {
            for (uint8_t j = 0; j < ns_len; j++) {
                if (EEPROM.read(offset + j) != (uint8_t)ns[j]) {
                    ns_match = false;
                    break;
                }
            }
        }
        offset += ns_len;

        if (offset + 1 > EEPROM_SIZE) return -2;
        uint8_t key_len = EEPROM.read(offset);
        offset++;

        if (offset + key_len > EEPROM_SIZE) return -2;

        // Compare key
        bool key_match = (strlen(key) == key_len);
        if (key_match) {
            for (uint8_t j = 0; j < key_len; j++) {
                if (EEPROM.read(offset + j) != (uint8_t)key[j]) {
                    key_match = false;
                    break;
                }
            }
        }
        offset += key_len;

        if (offset + 2 > EEPROM_SIZE) return -2;
        uint8_t type = EEPROM.read(offset);
        uint8_t val_len = EEPROM.read(offset + 1);

        if (offset + 2 + val_len > EEPROM_SIZE) return -2;

        // Skip tombstones
        if (type == 0xFF) {
            offset += 2 + val_len;
            continue;
        }

        if (ns_match && key_match && type == expected_type) {
            return offset + 2;  // Return value offset
        }

        offset += 2 + val_len;
    }

    return -1;  // Not found
}

// Helper: append a record to EEPROM
static bb_err_t append_record(const char *ns, const char *key, uint8_t type, const void *value, uint8_t val_len) {
    uint8_t ns_len = strlen(ns);
    uint8_t key_len = strlen(key);
    uint16_t needed = 5 + ns_len + key_len + val_len;  // 5 = ns_len + key_len + type + val_len

    // Find write position
    uint16_t count = (uint16_t)EEPROM.read(COUNT_OFFSET) | ((uint16_t)EEPROM.read(COUNT_OFFSET + 1) << 8);
    int offset = RECORD_START;

    // Scan to end
    for (uint16_t i = 0; i < count; i++) {
        if (offset + 5 > EEPROM_SIZE) return BB_ERR_INVALID_ARG;
        uint8_t nl = EEPROM.read(offset);
        offset += 1 + nl;
        if (offset + 1 > EEPROM_SIZE) return BB_ERR_INVALID_ARG;
        uint8_t kl = EEPROM.read(offset);
        offset += 1 + kl;
        if (offset + 2 > EEPROM_SIZE) return BB_ERR_INVALID_ARG;
        uint8_t vl = EEPROM.read(offset + 1);
        offset += 2 + vl;
    }

    if (offset + needed > EEPROM_SIZE) return BB_ERR_INVALID_ARG;  // No space

    // Write record
    EEPROM.write(offset, ns_len);
    offset++;
    for (uint8_t i = 0; i < ns_len; i++) EEPROM.write(offset + i, (uint8_t)ns[i]);
    offset += ns_len;

    EEPROM.write(offset, key_len);
    offset++;
    for (uint8_t i = 0; i < key_len; i++) EEPROM.write(offset + i, (uint8_t)key[i]);
    offset += key_len;

    EEPROM.write(offset, type);
    EEPROM.write(offset + 1, val_len);
    offset += 2;

    for (uint8_t i = 0; i < val_len; i++) EEPROM.write(offset + i, ((uint8_t*)value)[i]);

    // Increment count
    count++;
    EEPROM.write(COUNT_OFFSET, (uint8_t)count);
    EEPROM.write(COUNT_OFFSET + 1, (uint8_t)(count >> 8));

    return BB_OK;
}

// Helper: mark a record as tombstone
static bb_err_t tombstone_record(int val_offset) {
    // val_offset points to the value; type is 2 bytes before
    EEPROM.write(val_offset - 2, 0xFF);
    return BB_OK;
}

extern "C" bb_err_t bb_nv_set_u8(const char *ns, const char *key, uint8_t value) {
    if (!initialized) return BB_ERR_NOT_INITIALIZED;

    int val_offset = find_record(ns, key, 0);
    if (val_offset >= 0) {
        EEPROM.write(val_offset, value);
        return BB_OK;
    }

    // Not found, append
    return append_record(ns, key, 0, &value, 1);
}

extern "C" bb_err_t bb_nv_set_u32(const char *ns, const char *key, uint32_t value) {
    if (!initialized) return BB_ERR_NOT_INITIALIZED;

    int val_offset = find_record(ns, key, 1);
    if (val_offset >= 0) {
        EEPROM.write(val_offset, (uint8_t)value);
        EEPROM.write(val_offset + 1, (uint8_t)(value >> 8));
        EEPROM.write(val_offset + 2, (uint8_t)(value >> 16));
        EEPROM.write(val_offset + 3, (uint8_t)(value >> 24));
        return BB_OK;
    }

    // Not found, append
    uint8_t bytes[4] = {(uint8_t)value, (uint8_t)(value >> 8), (uint8_t)(value >> 16), (uint8_t)(value >> 24)};
    return append_record(ns, key, 1, bytes, 4);
}

extern "C" bb_err_t bb_nv_set_str(const char *ns, const char *key, const char *value) {
    if (!initialized) return BB_ERR_NOT_INITIALIZED;

    uint8_t str_len = strlen(value);

    int val_offset = find_record(ns, key, 2);
    if (val_offset >= 0) {
        // Check if size matches
        uint8_t old_len = EEPROM.read(val_offset - 1);
        if (old_len == str_len) {
            // Overwrite in place
            for (uint8_t i = 0; i < str_len; i++) {
                EEPROM.write(val_offset + i, (uint8_t)value[i]);
            }
            return BB_OK;
        }
        // Size changed, tombstone and append
        tombstone_record(val_offset);
    }

    return append_record(ns, key, 2, value, str_len);
}

extern "C" bb_err_t bb_nv_get_u8(const char *ns, const char *key, uint8_t *out, uint8_t fallback) {
    if (!initialized) return BB_ERR_NOT_INITIALIZED;

    int val_offset = find_record(ns, key, 0);
    if (val_offset < 0) {
        *out = fallback;
        return BB_ERR_NOT_FOUND;
    }

    *out = EEPROM.read(val_offset);
    return BB_OK;
}

extern "C" bb_err_t bb_nv_get_u32(const char *ns, const char *key, uint32_t *out, uint32_t fallback) {
    if (!initialized) return BB_ERR_NOT_INITIALIZED;

    int val_offset = find_record(ns, key, 1);
    if (val_offset < 0) {
        *out = fallback;
        return BB_ERR_NOT_FOUND;
    }

    uint32_t val = (uint32_t)EEPROM.read(val_offset)
                 | ((uint32_t)EEPROM.read(val_offset + 1) << 8)
                 | ((uint32_t)EEPROM.read(val_offset + 2) << 16)
                 | ((uint32_t)EEPROM.read(val_offset + 3) << 24);
    *out = val;
    return BB_OK;
}

extern "C" bb_err_t bb_nv_get_str(const char *ns, const char *key, char *buf, size_t len, const char *fallback) {
    if (!initialized) return BB_ERR_NOT_INITIALIZED;

    int val_offset = find_record(ns, key, 2);
    if (val_offset < 0) {
        if (fallback && strlen(fallback) < len) {
            strcpy(buf, fallback);
        }
        return BB_ERR_NOT_FOUND;
    }

    uint8_t str_len = EEPROM.read(val_offset - 1);
    if (str_len >= len) {
        return BB_ERR_INVALID_ARG;  // Buffer too small
    }

    for (uint8_t i = 0; i < str_len; i++) {
        buf[i] = (char)EEPROM.read(val_offset + i);
    }
    buf[str_len] = '\0';

    return BB_OK;
}

extern "C" bb_err_t bb_nv_erase(const char *ns, const char *key) {
    if (!initialized) return BB_ERR_NOT_INITIALIZED;

    // Try each type
    for (uint8_t type = 0; type <= 2; type++) {
        int val_offset = find_record(ns, key, type);
        if (val_offset >= 0) {
            tombstone_record(val_offset);
            return BB_OK;
        }
    }

    return BB_ERR_NOT_FOUND;
}

// Placeholder stubs for ESP-gated functions (not called on Arduino)
extern "C" const char *bb_nv_config_wifi_ssid(void) { return ""; }
extern "C" const char *bb_nv_config_wifi_pass(void) { return ""; }
extern "C" bool bb_nv_config_display_enabled(void) { return false; }
