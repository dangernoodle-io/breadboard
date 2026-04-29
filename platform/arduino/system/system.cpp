#ifdef ARDUINO

#include "bb_http.h"

#include <Arduino.h>
#include <string.h>

#if defined(__AVR__)
#include <avr/wdt.h>
#elif defined(__arm__)
// Cortex-M (Uno R4 / Renesas RA, SAMD, etc.) — NVIC_SystemReset via CMSIS.
#endif

#ifndef BB_FIRMWARE_VERSION
#define BB_FIRMWARE_VERSION "unknown"
#endif

void bb_system_restart(void)
{
    delay(500);
#if defined(__AVR__)
    wdt_enable(WDTO_15MS);
    for (;;) {}
#elif defined(__arm__)
    NVIC_SystemReset();
    for (;;) {}
#else
    for (;;) {}
#endif
}

#endif
