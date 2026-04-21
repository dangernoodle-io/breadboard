#ifdef ARDUINO

#include "bb_http.h"

#include <Arduino.h>
#include <string.h>
#include <avr/wdt.h>

#ifndef BB_FIRMWARE_VERSION
#define BB_FIRMWARE_VERSION "unknown"
#endif

void bb_system_get_version(char *out, size_t out_size)
{
    if (!out || out_size == 0) return;
    strncpy(out, BB_FIRMWARE_VERSION, out_size - 1);
    out[out_size - 1] = '\0';
}

void bb_system_reboot(void)
{
    delay(500);
    wdt_enable(WDTO_15MS);
    for (;;) {}
}

#endif
