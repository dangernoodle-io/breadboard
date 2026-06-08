#include "bb_diag.h"

void bb_diag_scrub_text(char *s)
{
    if (!s) return;

    for (unsigned char *p = (unsigned char *)s; *p != '\0'; p++) {
        unsigned char c = *p;
        /* allow: \t (0x09), \n (0x0A), \r (0x0D), printable ASCII (0x20..0x7E) */
        if (c == 0x09 || c == 0x0A || c == 0x0D || (c >= 0x20 && c <= 0x7E)) {
            continue;
        }
        *p = '?';
    }
}
