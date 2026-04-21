#include <Arduino.h>
#include <stdarg.h>
#include <stdio.h>

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
