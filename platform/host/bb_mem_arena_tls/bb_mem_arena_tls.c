/* bb_mem_arena_tls — host stub.
 *
 * Simulates arena routing behavior for testing without ESP-IDF deps.
 * Rebuilt on the generic bb_mem_arena primitive (components/bb_mem_arena) — the
 * arena mechanics (bump-alloc, owns, reset, stats) are shared with the
 * ESP-IDF impl; only the mbedTLS-shaped calloc/free routing and Kconfig
 * bridge live here. Arena size is set via CONFIG_BB_MEM_ARENA_TLS_BYTES in
 * build flags. Exposes bb_mem_arena_tls_calloc/free/test_reset under
 * BB_MEM_ARENA_TLS_TESTING.
 */
#include "bb_mem_arena_tls.h"
#include "bb_mem_arena.h"
#include "bb_mem.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <pthread.h>

/* Kconfig bridge: honour CONFIG_BB_MEM_ARENA_TLS_BYTES from build flags; default 0. */
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif
#ifdef CONFIG_BB_MEM_ARENA_TLS_BYTES
#define BB_MEM_ARENA_TLS_BYTES CONFIG_BB_MEM_ARENA_TLS_BYTES
#endif
#ifndef BB_MEM_ARENA_TLS_BYTES
#define BB_MEM_ARENA_TLS_BYTES 0
#endif

#if BB_MEM_ARENA_TLS_BYTES > 0

/* Static backing buffer for the bb_mem_arena instance used by host tests. */
static uint8_t s_arena_buf[BB_MEM_ARENA_TLS_BYTES] __attribute__((aligned(16)));
static bb_mem_arena_t s_arena = NULL;
/* Count of arena-owned allocations currently outstanding (not yet freed).
 * When this drains to 0 in the free path, the arena is reset so the bump
 * pointer rewinds and the next handshake can reuse the whole region —
 * without this, the bump allocator never reclaims and every handshake
 * after the first silently falls through to the heap fallback forever. */
static size_t s_arena_outstanding;
#define ARENA_ENABLED 1

#else
#define ARENA_ENABLED 0
#endif /* BB_MEM_ARENA_TLS_BYTES > 0 */

/* Process-wide mbedTLS allocator — mirrors the ESP-IDF impl. The arena's
 * bump-offset write and s_arena_outstanding inc/dec + conditional reset are
 * shared state that MUST be serialized across concurrent handshakes;
 * without this mutex, two concurrent callers can corrupt the bump offset or
 * race the counter to a premature reset (use-after-free). Held across the
 * ENTIRE body of both hooks so there is exactly one unlock+return path in
 * each. Concurrency itself is not host-unit-testable — the mutex is the
 * guard; this test binary only exercises the sequential single-caller
 * routing/reset logic. */
static pthread_mutex_t s_arena_mtx = PTHREAD_MUTEX_INITIALIZER;
static bool s_inited;

void bb_mem_arena_tls_init(void)
{
    pthread_mutex_lock(&s_arena_mtx);
    if (!s_inited) {
#if ARENA_ENABLED
        bb_mem_arena_init(&s_arena, s_arena_buf, sizeof(s_arena_buf));
        s_arena_outstanding = 0;
#endif
        s_inited = true;
    }
    pthread_mutex_unlock(&s_arena_mtx);
}

bool bb_mem_arena_tls_owns(const void *ptr)
{
    pthread_mutex_lock(&s_arena_mtx);
#if ARENA_ENABLED
    bool owns = bb_mem_arena_owns(s_arena, ptr);
#else
    (void)ptr;
    bool owns = false;
#endif
    pthread_mutex_unlock(&s_arena_mtx);
    return owns;
}

#if defined(BB_MEM_ARENA_TLS_TESTING)

void *bb_mem_arena_tls_calloc(size_t n, size_t size)
{
    size_t total = n * size;
    if (total == 0) return NULL;

    pthread_mutex_lock(&s_arena_mtx);
    void *ret = NULL;
#if ARENA_ENABLED
    if (s_arena != NULL) {
        ret = bb_mem_arena_alloc(s_arena, total);
        if (ret) {
            memset(ret, 0, total);
            s_arena_outstanding++;
        }
    }
#endif
    if (ret == NULL) {
        /* Fallback: internal-heap facade (mirrors the ESP-IDF impl's routing). */
        ret = bb_calloc_internal(n, size);
    }
    pthread_mutex_unlock(&s_arena_mtx);
    return ret;
}

void bb_mem_arena_tls_free(void *ptr)
{
    if (!ptr) return;

    pthread_mutex_lock(&s_arena_mtx);
#if ARENA_ENABLED
    if (s_arena != NULL && bb_mem_arena_owns(s_arena, ptr)) {
        bb_mem_arena_free(s_arena, ptr);
        /* NOTE: this guard prevents size_t underflow from a caller
         * double-free, but cannot detect the double-free itself in release
         * builds (bb_mem_arena_owns is range-only; the debug assert inside
         * bb_mem_arena_free is compiled out under NDEBUG). mbedTLS is a
         * well-behaved single-free caller in practice; per-allocation
         * liveness tracking would be a bb_mem_arena change, out of scope here. */
        if (s_arena_outstanding > 0) {
            s_arena_outstanding--;
        }
        if (s_arena_outstanding == 0) {
            /* Arena fully drained — rewind so the next handshake can reuse
             * the whole region instead of permanently falling back to heap. */
            bb_mem_arena_reset(s_arena);
        }
    } else {
        bb_mem_free(ptr);
    }
#else
    bb_mem_free(ptr);
#endif
    pthread_mutex_unlock(&s_arena_mtx);
}

void bb_mem_arena_tls_test_reset(void)
{
    pthread_mutex_lock(&s_arena_mtx);
#if ARENA_ENABLED
    bb_mem_arena_reset(s_arena);
    s_arena_outstanding = 0;
#endif
    s_inited = false;
    pthread_mutex_unlock(&s_arena_mtx);
}

#endif /* BB_MEM_ARENA_TLS_TESTING */
