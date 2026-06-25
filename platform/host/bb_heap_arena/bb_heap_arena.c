/* bb_heap_arena — host stub.
 *
 * Simulates arena routing behavior for testing without ESP-IDF deps.
 * Arena size is set via CONFIG_BB_HEAP_ARENA_BYTES in build flags.
 * Exposes bb_heap_arena_calloc/free/test_reset under BB_HEAP_ARENA_TESTING.
 */
#include "bb_heap_arena.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Kconfig bridge: honour CONFIG_BB_HEAP_ARENA_BYTES from build flags; default 0. */
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif
#ifdef CONFIG_BB_HEAP_ARENA_BYTES
#define BB_HEAP_ARENA_BYTES CONFIG_BB_HEAP_ARENA_BYTES
#endif
#ifndef BB_HEAP_ARENA_BYTES
#define BB_HEAP_ARENA_BYTES 0
#endif

#if BB_HEAP_ARENA_BYTES > 0

/* Static arena buffer for host tests. */
static uint8_t s_arena[BB_HEAP_ARENA_BYTES] __attribute__((aligned(8)));
#define ARENA_SIZE BB_HEAP_ARENA_BYTES

/* Minimal bump allocator with freed-block tracking.
 * Block layout: [hdr_t | payload | pad-to-8]
 * Free sets used=0 (marks as freed; pointer remains in-range for owns()). */
typedef struct {
    size_t  size;
    uint8_t used;
} hdr_t;

/* Round header up to 8-byte boundary so payload stays aligned. */
#define HDR_SZ ((sizeof(hdr_t) + 7u) & ~7u)

static size_t s_offset      = 0;
static bool   s_initialized = false;

static void *arena_alloc(size_t bytes)
{
    size_t needed = (HDR_SZ + bytes + 7u) & ~7u;
    if (s_offset + needed > ARENA_SIZE) return NULL;
    hdr_t *h = (hdr_t *)(s_arena + s_offset);
    h->size  = bytes;
    h->used  = 1;
    s_offset += needed;
    return (uint8_t *)h + HDR_SZ;
}

static void arena_free(void *ptr)
{
    hdr_t *h = (hdr_t *)((uint8_t *)ptr - HDR_SZ);
    h->used  = 0;
}

#define ARENA_ENABLED 1
#else
#define ARENA_ENABLED 0
#define ARENA_SIZE    0
#endif /* BB_HEAP_ARENA_BYTES > 0 */

void bb_heap_arena_init(void)
{
#if ARENA_ENABLED
    s_offset      = 0;
    s_initialized = true;
#endif
}

bool bb_heap_arena_owns(const void *ptr)
{
#if ARENA_ENABLED
    return (ptr >= (const void *)s_arena &&
            ptr < (const void *)(s_arena + ARENA_SIZE));
#else
    (void)ptr;
    return false;
#endif
}

#if defined(BB_HEAP_ARENA_TESTING)

void *bb_heap_arena_calloc(size_t n, size_t size)
{
    size_t total = n * size;
    if (total == 0) return NULL;

#if ARENA_ENABLED
    if (s_initialized) {
        void *p = arena_alloc(total);
        if (p) {
            memset(p, 0, total);
            return p;
        }
    }
#endif
    /* Fallback to stdlib calloc on host. */
    return calloc(n, size);
}

void bb_heap_arena_free(void *ptr)
{
    if (!ptr) return;
#if ARENA_ENABLED
    if ((void *)ptr >= (void *)s_arena &&
        (void *)ptr < (void *)(s_arena + ARENA_SIZE)) {
        arena_free(ptr);
        return;
    }
#endif
    free(ptr);
}

void bb_heap_arena_test_reset(void)
{
#if ARENA_ENABLED
    s_offset      = 0;
    s_initialized = false;
#endif
}

#endif /* BB_HEAP_ARENA_TESTING */
