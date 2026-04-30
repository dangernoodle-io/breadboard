#ifdef ARDUINO

#include <Arduino.h>
#include "smoke_app.h"

void setup() {
    Serial.begin(115200);
    while (!Serial);

    smoke_app_setup();
}

void loop() {
    smoke_app_loop();
    delay(10);
}

#endif
