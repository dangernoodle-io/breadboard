// Host platform implementation of bb_cache.
//
// The espidf implementation uses only pthreads and standard C (no FreeRTOS /
// esp-specific APIs), so the full registry + serializer logic compiles on host
// unchanged. We simply include it here so the host build gets the real impl.

#include "../../espidf/bb_cache/bb_cache_espidf.c"
