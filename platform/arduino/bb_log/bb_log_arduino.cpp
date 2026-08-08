#include <Arduino.h>
#include <stdarg.h>
#include <stdio.h>
#include "bb_core.h"  // bb_err_t/BB_OK -- extern "C"-wrapped, safe to include from a .cpp
#include "bb_log.h"   // bb_log_drop_stats_t -- extern "C"-wrapped, safe to include from a .cpp

extern "C" void bb_log_arduino_emit(char level, const char *tag, const char *fmt, ...) {
    char buf[64];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (n < 0) return;
    Serial.print(level);
    Serial.print(F(" ("));
    Serial.print(tag);
    Serial.print(F(") "));
    Serial.println(buf);
}

// bb_log_arduino_emit above writes straight through Serial.print/println --
// nothing is queued asynchronously, so flushing just needs to make sure the
// Serial TX buffer itself has drained.
//
// KNOWN DIVERGENCE from bb_log.h's documented "never blocks forever"
// contract (see its doc comment): Serial.flush()'s own contract across the
// Arduino cores this repo targets (AVR/Uno+CC3000, Renesas Arduino UNO R4
// WiFi) is to block until the TX buffer drains with NO timeout of its own,
// so an unplugged/stalled USB host CAN hang this call. A bounded
// alternative (poll Serial.availableForWrite() against a millis()
// deadline) was considered instead, but neither core exposes a portable
// "TX buffer capacity" constant to treat availableForWrite()'s return value
// as a reliable "drained" proxy, and this divergence is unverifiable in the
// task that added it (no Arduino hardware in scope) -- rather than ship an
// unverified, possibly-wrong bounded implementation, the divergence is
// scoped honestly here and in bb_log.h instead.
extern "C" bb_err_t bb_log_flush(void)
{
    Serial.flush();
    return BB_OK;
}

// Arduino has no async writer/UDP/event forwarder queue or flush-timeout
// mechanism (bb_log_arduino_emit above writes synchronously) -- every field
// always reads 0.
extern "C" void bb_log_drop_stats_get(bb_log_drop_stats_t *out)
{
    if (!out) return;

    out->writer_dropped = 0;
    out->udp_dropped     = 0;
    out->event_dropped   = 0;
    out->flush_timeouts  = 0;
}
