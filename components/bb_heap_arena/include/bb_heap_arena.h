#pragma once
#include <stdbool.h>
#include "bb_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * bb_heap_arena — boot-reserved contiguous mbedTLS handshake arena.
 *
 * ORDERING CONTRACT (CRITICAL):
 *   Call bb_heap_arena_init() at the very top of app_main(), BEFORE
 *   bb_init_init_early(). The mbedTLS allocator must be installed before
 *   any WiFi/WPA-supplicant mbedTLS use (which occurs during esp_wifi_connect
 *   inside the EARLY-tier bb_wifi init). Calling this function after
 *   bb_init_init_early() is undefined behavior.
 *
 * When CONFIG_MBEDTLS_CUSTOM_MEM_ALLOC is NOT set, this function is a no-op
 * and ESP-IDF's default mbedTLS allocator (esp_mem.c) handles all allocations.
 *
 * When CONFIG_MBEDTLS_CUSTOM_MEM_ALLOC IS set:
 *   - Installs bb_heap_arena_calloc/bb_heap_arena_free as the global mbedTLS
 *     allocator via mbedtls_platform_set_calloc_free().
 *   - If CONFIG_BB_HEAP_ARENA_BYTES > 0: reserves a static contiguous buffer
 *     of that size and serves allocations arena-first (try-arena, fall back to
 *     heap_caps_calloc(INTERNAL|8BIT) when exhausted).
 *   - If CONFIG_BB_HEAP_ARENA_BYTES == 0: no arena buffer; calloc/free route
 *     directly to heap_caps_calloc(INTERNAL|8BIT) / heap_caps_free().
 */
void bb_heap_arena_init(void);

/**
 * Returns true if ptr falls within the arena buffer.
 * Returns false if no arena is configured (BYTES == 0) or CUSTOM_MEM_ALLOC is off.
 * Host-testable pure predicate.
 */
bool bb_heap_arena_owns(const void *ptr);

#if defined(BB_HEAP_ARENA_TESTING)
/* Test hooks: exercise routing without going through mbedtls_platform. */
void *bb_heap_arena_calloc(size_t n, size_t size);
void  bb_heap_arena_free(void *ptr);
/* Reset arena state (re-run init from scratch) for test isolation. */
void  bb_heap_arena_test_reset(void);
#endif

#ifdef __cplusplus
}
#endif
