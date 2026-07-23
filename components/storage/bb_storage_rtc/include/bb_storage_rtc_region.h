#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Magic number identifying a valid bb_storage_rtc_region_t blob ('BBNC').
 * Chosen to be distinctive and unlikely to appear in uninitialised RTC
 * memory. RELOCATED verbatim from bb_nv_creds_mirror (B1-242 lineage) —
 * MUST NOT change: an OTA'd board must keep validating a region written by
 * the pre-relocation code (bb_nv's own mirror keeps the identical value).
 */
#define BB_STORAGE_RTC_REGION_MAGIC   0x42424E43u

/**
 * Current struct version. Increment when the layout changes.
 * bb_storage_rtc_region_valid() rejects any version != current. MUST NOT
 * change without a deliberate migration — see BB_STORAGE_RTC_REGION_MAGIC.
 */
#define BB_STORAGE_RTC_REGION_VERSION 1u

/**
 * Packed mirror of WiFi credentials stored in RTC slow memory.
 *
 * Field layout is deterministic (no compiler-inserted padding beyond _pad):
 *   magic       - sentinel; must equal BB_STORAGE_RTC_REGION_MAGIC
 *   version     - struct layout version; must equal BB_STORAGE_RTC_REGION_VERSION
 *   provisioned - 1 if the device has been provisioned, 0 otherwise
 *   _pad        - explicit alignment pad; zero-filled by pack; keeps CRC stable
 *   ssid        - NUL-terminated WiFi SSID (max 31 chars + NUL)
 *   pass        - NUL-terminated WiFi password (max 63 chars + NUL)
 *   crc         - CRC-32 over all preceding bytes (offsetof(…,crc) bytes)
 *
 * The crc field covers every byte from magic through the last byte of pass,
 * so a single-bit flip anywhere in those fields is detected by valid().
 *
 * Callers that restore credentials from this region MUST additionally check
 * that ssid[0] != '\0' before using the credentials; an empty SSID means the
 * device was provisioned with no network configured and is not restorable.
 * bb_storage_rtc_region_valid() does NOT enforce this — the struct is still
 * structurally valid with an empty SSID.
 *
 * Capacity coupling: ssid[32]/pass[64] below must stay byte-compatible with
 * bb_settings' s_wifi_ssid_field.max_len/s_wifi_pass_field.max_len
 * (platform/host/bb_settings/bb_settings.c) or bb_settings' RTC-mirror write
 * silently BB_ERR_NO_SPACE-fails against this region. Not compiler-enforced
 * on purpose (bb_settings must not gain a direct bb_storage_rtc include/
 * dependency — see bb_settings.c's mirror-write comment) — editing either
 * side, check the other.
 */
typedef struct {
    uint32_t magic;        /**< BB_STORAGE_RTC_REGION_MAGIC */
    uint16_t version;      /**< BB_STORAGE_RTC_REGION_VERSION */
    uint8_t  provisioned;  /**< 0 = not provisioned, 1 = provisioned */
    uint8_t  _pad;         /**< reserved; always zero; keeps CRC deterministic */
    char     ssid[32];     /**< NUL-terminated SSID, truncated to 31 chars */
    char     pass[64];     /**< NUL-terminated password, truncated to 63 chars */
    uint32_t crc;          /**< CRC-32 over all bytes before this field */
} bb_storage_rtc_region_t;

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
void bb_storage_rtc_region_pack(bb_storage_rtc_region_t *out,
                                 const char *ssid,
                                 const char *pass,
                                 uint8_t provisioned);

/**
 * Validate a region blob.
 *
 * Returns true iff:
 *   - @p r is not NULL
 *   - r->magic == BB_STORAGE_RTC_REGION_MAGIC
 *   - r->version == BB_STORAGE_RTC_REGION_VERSION
 *   - CRC recomputed over the first offsetof(bb_storage_rtc_region_t,crc)
 *     bytes matches r->crc
 *
 * @param r  Region to validate; may be NULL (returns false).
 */
bool bb_storage_rtc_region_valid(const bb_storage_rtc_region_t *r);

/**
 * Compute the CRC-32 over the region blob excluding the trailing crc field.
 *
 * Uses a software CRC-32 with reflected polynomial 0xEDB88320, init
 * 0xFFFFFFFF, final XOR 0xFFFFFFFF (standard Ethernet/zlib CRC-32).
 *
 * Exposed for unit tests; use bb_storage_rtc_region_pack and
 * bb_storage_rtc_region_valid rather than calling this directly.
 *
 * @param r  Source struct; must not be NULL.
 * @return   32-bit CRC value.
 */
uint32_t bb_storage_rtc_region_crc(const bb_storage_rtc_region_t *r);

#ifdef __cplusplus
}
#endif
