#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Magic number identifying a valid bb_nv_creds_mirror_t blob ('BBNC').
 * Chosen to be distinctive and unlikely to appear in uninitialised RTC memory.
 */
#define BB_NV_CREDS_MIRROR_MAGIC   0x42424E43u

/**
 * Current struct version. Increment when the layout changes.
 * bb_nv_creds_mirror_valid() rejects any version != current.
 */
#define BB_NV_CREDS_MIRROR_VERSION 1u

/**
 * Packed mirror of WiFi credentials stored in RTC slow memory.
 *
 * Field layout is deterministic (no compiler-inserted padding beyond _pad):
 *   magic       - sentinel; must equal BB_NV_CREDS_MIRROR_MAGIC
 *   version     - struct layout version; must equal BB_NV_CREDS_MIRROR_VERSION
 *   provisioned - 1 if the device has been provisioned, 0 otherwise
 *   _pad        - explicit alignment pad; zero-filled by pack; keeps CRC stable
 *   ssid        - NUL-terminated WiFi SSID (max 31 chars + NUL)
 *   pass        - NUL-terminated WiFi password (max 63 chars + NUL)
 *   crc         - CRC-32 over all preceding bytes (offsetof(…,crc) bytes)
 *
 * The crc field covers every byte from magic through the last byte of pass,
 * so a single-bit flip anywhere in those fields is detected by valid().
 *
 * Callers that restore credentials from this mirror MUST additionally check
 * that ssid[0] != '\0' before using the credentials; an empty SSID means the
 * device was provisioned with no network configured and is not restorable.
 * bb_nv_creds_mirror_valid() does NOT enforce this — the struct is still
 * structurally valid with an empty SSID.
 */
typedef struct {
    uint32_t magic;        /**< BB_NV_CREDS_MIRROR_MAGIC */
    uint16_t version;      /**< BB_NV_CREDS_MIRROR_VERSION */
    uint8_t  provisioned;  /**< 0 = not provisioned, 1 = provisioned */
    uint8_t  _pad;         /**< reserved; always zero; keeps CRC deterministic */
    char     ssid[32];     /**< NUL-terminated SSID, truncated to 31 chars */
    char     pass[64];     /**< NUL-terminated password, truncated to 63 chars */
    uint32_t crc;          /**< CRC-32 over all bytes before this field */
} bb_nv_creds_mirror_t;

/**
 * Populate @p out with the supplied credentials and stamp a valid CRC.
 *
 * Zeroes the entire struct first, then sets magic, version, provisioned,
 * copies ssid/pass with bounded copy (always NUL-terminated; truncated if
 * longer than the field allows), and computes crc last.
 *
 * @param out         Destination struct; must not be NULL.
 * @param ssid        Source SSID string; NULL treated as "".
 * @param pass        Source password string; NULL treated as "".
 * @param provisioned 1 if provisioned, 0 otherwise.
 */
void bb_nv_creds_mirror_pack(bb_nv_creds_mirror_t *out,
                              const char *ssid,
                              const char *pass,
                              uint8_t provisioned);

/**
 * Validate a mirror blob.
 *
 * Returns true iff:
 *   - @p m is not NULL
 *   - m->magic == BB_NV_CREDS_MIRROR_MAGIC
 *   - m->version == BB_NV_CREDS_MIRROR_VERSION
 *   - CRC recomputed over the first offsetof(bb_nv_creds_mirror_t,crc) bytes
 *     matches m->crc
 *
 * @param m  Mirror to validate; may be NULL (returns false).
 */
bool bb_nv_creds_mirror_valid(const bb_nv_creds_mirror_t *m);

/**
 * Compute the CRC-32 over the mirror blob excluding the trailing crc field.
 *
 * Uses a software CRC-32 with reflected polynomial 0xEDB88320, init
 * 0xFFFFFFFF, final XOR 0xFFFFFFFF (standard Ethernet/zlib CRC-32).
 *
 * Exposed for unit tests; use bb_nv_creds_mirror_pack and
 * bb_nv_creds_mirror_valid rather than calling this directly.
 *
 * @param m  Source struct; must not be NULL.
 * @return   32-bit CRC value.
 */
uint32_t bb_nv_creds_mirror_crc(const bb_nv_creds_mirror_t *m);

#ifdef __cplusplus
}
#endif
