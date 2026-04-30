#pragma once

// Portable smoke app entry points. Implemented in smoke_app.c, called
// from the per-framework entry shim (entry_arduino.cpp / entry_espidf.c).

#ifdef __cplusplus
extern "C" {
#endif

void smoke_app_setup(void);
void smoke_app_loop(void);

#ifdef __cplusplus
}
#endif
