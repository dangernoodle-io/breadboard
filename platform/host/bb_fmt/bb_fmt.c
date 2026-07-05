// bb_fmt — portable formatting helpers (hex encode, MAC address, bool).
//
// Compiled on both host (tests) and ESP-IDF. Pure C, no platform deps.

#include "bb_fmt.h"

static const char s_hex_digits[] = "0123456789abcdef";

size_t bb_fmt_hex(const uint8_t *bytes, size_t nbytes, char sep, char *dst, size_t dstsize)
{
    size_t full_len = 0;
    for (size_t i = 0; i < nbytes; i++) {
        if (i > 0 && sep != 0) {
            full_len++;
        }
        full_len += 2;
    }

    if (dstsize == 0) {
        return full_len;
    }

    size_t pos = 0;
    size_t last = dstsize - 1;

    for (size_t i = 0; i < nbytes; i++) {
        if (i > 0 && sep != 0) {
            if (pos < last) {
                dst[pos] = sep;
            }
            pos++;
        }

        uint8_t b = bytes[i];

        if (pos < last) {
            dst[pos] = s_hex_digits[(b >> 4) & 0x0F];
        }
        pos++;

        if (pos < last) {
            dst[pos] = s_hex_digits[b & 0x0F];
        }
        pos++;
    }

    size_t term_pos = (pos < last) ? pos : last;
    dst[term_pos] = '\0';

    return full_len;
}

bool bb_fmt_mac6(const uint8_t mac[6], char *dst, size_t dstsize)
{
    if (dstsize < 18) {
        return false;
    }

    // Return value discarded: dstsize >= 18 above guarantees the 17-char
    // "xx:xx:xx:xx:xx:xx" result never truncates.
    bb_fmt_hex(mac, 6, ':', dst, dstsize);
    return true;
}

const char *bb_fmt_bool(bool v)
{
    return v ? "true" : "false";
}
