// Host platform implementation of bb_cache_reactive.
//
// The espidf implementation uses only pthreads and standard C (no FreeRTOS /
// esp-specific APIs), so the full observer-pool + fire logic compiles on
// host unchanged. We simply include it here so the host build gets the real
// impl -- same pattern as platform/host/bb_cache/bb_cache_host.c.

#include "../../espidf/bb_cache_reactive/bb_cache_reactive_espidf.c"
