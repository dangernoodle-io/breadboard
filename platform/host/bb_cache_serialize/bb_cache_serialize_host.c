// Host platform implementation of bb_cache_serialize.
//
// The espidf implementation uses only pthreads and standard C (no FreeRTOS /
// esp-specific APIs), so the full slot-table logic compiles on host
// unchanged. We simply include it here so the host build gets the real impl.

#include "../../espidf/bb_cache_serialize/bb_cache_serialize.c"
