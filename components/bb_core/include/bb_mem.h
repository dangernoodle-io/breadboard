#pragma once

// bb_mem — SPIRAM-preferred allocation helpers.
//
// On ESP-IDF these try the SPIRAM/8-bit heap first and fall back to the
// default (internal) heap, keeping large/long-lived buffers out of the scarce
// internal-RAM budget shared with TLS handshakes and real-time stacks; on
// boards without PSRAM the fallback preserves the original behaviour. On host
// they are plain malloc/calloc/free.
//
// Allocations returned by bb_malloc_prefer_spiram / bb_calloc_prefer_spiram
// MUST be released with bb_mem_free (not libc free), since the ESP-IDF backend
// allocates via heap_caps and frees via heap_caps_free.

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Allocate `size` bytes, preferring SPIRAM with fallback to the default heap.
// Returns NULL on failure. Contents are uninitialised (malloc semantics).
void *bb_malloc_prefer_spiram(size_t size);

// Allocate `n * size` zero-initialised bytes, preferring SPIRAM with fallback
// to the default heap. Returns NULL on failure.
void *bb_calloc_prefer_spiram(size_t n, size_t size);

// Free a pointer returned by the bb_*_prefer_spiram helpers. NULL is a no-op.
void bb_mem_free(void *p);

#ifdef __cplusplus
}
#endif
