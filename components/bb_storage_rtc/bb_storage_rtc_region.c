#include "bb_storage_rtc_region.h"

#include <stddef.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Portable software CRC-32 (reflected polynomial 0xEDB88320).
 * init = 0xFFFFFFFF, final XOR = 0xFFFFFFFF — standard Ethernet / zlib CRC-32.
 * No global table; each call recomputes on-the-fly to keep BSS clean.
 * ---------------------------------------------------------------------------*/
static uint32_t crc32_byte(uint32_t crc, uint8_t b)
{
    crc ^= b;
    for (int i = 0; i < 8; i++) {
        crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1u));
    }
    return crc;
}

static uint32_t crc32_buf(const uint8_t *buf, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc = crc32_byte(crc, buf[i]);
    }
    return crc ^ 0xFFFFFFFFu;
}

/* ---------------------------------------------------------------------------
 * Bounded string copy: copies at most dst_cap-1 bytes from src into dst,
 * always NUL-terminates. Safe when src is NULL (writes empty string).
 * Avoids relying on strlcpy which is not available on all host toolchains.
 * ---------------------------------------------------------------------------*/
/* Precondition: dst_cap >= 1 (callers pass sizeof of fixed region buffers). */
static void bounded_copy(char *dst, const char *src, size_t dst_cap)
{
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    size_t i = 0;
    while (i < dst_cap - 1 && src[i] != '\0') {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

/* ---------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------------*/

uint32_t bb_storage_rtc_region_crc(const bb_storage_rtc_region_t *r)
{
    return crc32_buf((const uint8_t *)r, offsetof(bb_storage_rtc_region_t, crc));
}

void bb_storage_rtc_region_pack(bb_storage_rtc_region_t *out,
                                 const char *ssid,
                                 const char *pass,
                                 uint8_t provisioned)
{
    memset(out, 0, sizeof(*out));
    out->magic       = BB_STORAGE_RTC_REGION_MAGIC;
    out->version     = BB_STORAGE_RTC_REGION_VERSION;
    out->provisioned = provisioned;
    /* _pad is already zero from memset */
    bounded_copy(out->ssid, ssid, sizeof(out->ssid));
    bounded_copy(out->pass, pass, sizeof(out->pass));
    out->crc = bb_storage_rtc_region_crc(out);
}

bool bb_storage_rtc_region_valid(const bb_storage_rtc_region_t *r)
{
    if (r == NULL) {
        return false;
    }
    if (r->magic != BB_STORAGE_RTC_REGION_MAGIC) {
        return false;
    }
    if (r->version != BB_STORAGE_RTC_REGION_VERSION) {
        return false;
    }
    return bb_storage_rtc_region_crc(r) == r->crc;
}
