#include "bb_mem_arena_tls.h"
#include "bb_mem_arena.h"
#include "bb_mem.h"
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <pthread.h>

#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif

/* Guard: if arena bytes > 0 but MBEDTLS_CUSTOM_MEM_ALLOC is not set, the
 * static arena buffer is never registered as an allocator — all those bytes
 * sit unused in BSS forever.  Catch this misconfiguration at compile time. */
#if defined(CONFIG_BB_MEM_ARENA_TLS_BYTES) && (CONFIG_BB_MEM_ARENA_TLS_BYTES > 0) && \
    !defined(CONFIG_MBEDTLS_CUSTOM_MEM_ALLOC)
#  error "BB_MEM_ARENA_TLS_BYTES > 0 requires CONFIG_MBEDTLS_CUSTOM_MEM_ALLOC=y " \
         "(arena allocated but mbedTLS will never use it — wasted BSS); " \
         "set CONFIG_MBEDTLS_CUSTOM_MEM_ALLOC=y or set CONFIG_BB_MEM_ARENA_TLS_BYTES=0"
#endif

/* Only install when CONFIG_MBEDTLS_CUSTOM_MEM_ALLOC is set.
 * When NOT set, esp_mem.c (ESP-IDF) handles the allocator; we install nothing. */
#if defined(CONFIG_MBEDTLS_CUSTOM_MEM_ALLOC)

#include "mbedtls/platform.h"

#if defined(CONFIG_BB_MEM_ARENA_TLS_BYTES) && CONFIG_BB_MEM_ARENA_TLS_BYTES > 0
/* Static backing buffer for the bb_mem_arena instance — aligned for
 * max_align_t so the carved-off bb_mem_arena header and every allocation
 * satisfy worst-case alignment. */
static uint8_t s_arena_buf[CONFIG_BB_MEM_ARENA_TLS_BYTES] __attribute__((aligned(16)));
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
#endif

/* Process-wide mbedTLS allocator — this codebase runs concurrent TLS
 * (telemetry sink + OTA/update-check, WPA-supplicant). The arena's
 * bump-offset write and s_arena_outstanding inc/dec + conditional reset are
 * shared state that MUST be serialized across concurrent handshakes;
 * without this mutex, two concurrent callers can corrupt the bump offset or
 * race the counter to a premature reset (use-after-free). Held across the
 * ENTIRE body of both hooks — including the heap fallback — so there is
 * exactly one unlock+return path in each. */
static pthread_mutex_t s_arena_mtx = PTHREAD_MUTEX_INITIALIZER;
static bool s_installed;

static void *bb_mem_arena_tls_calloc_impl(size_t n, size_t size)
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
        /* Fallback: internal 8-bit heap (matches ESP-IDF default in esp_mem.c) */
        ret = bb_calloc_internal(n, size);
    }
    pthread_mutex_unlock(&s_arena_mtx);
    return ret;
}

static void bb_mem_arena_tls_free_impl(void *ptr)
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

void bb_mem_arena_tls_init(void)
{
    pthread_mutex_lock(&s_arena_mtx);
#if ARENA_ENABLED
    if (s_arena == NULL) {
        bb_mem_arena_init(&s_arena, s_arena_buf, sizeof(s_arena_buf));
        s_arena_outstanding = 0;
        /* Install custom allocator (required when CUSTOM_MEM_ALLOC=y —
         * ESP-IDF compiles out esp_mem.c, leaving no default; we MUST
         * install one). Installing only on first-init makes a second call
         * (e.g. explicit app_main() call + CONFIG_BB_MEM_ARENA_TLS_AUTOREGISTER)
         * a true no-op — it cannot zero the counter or rewind the arena
         * while allocations from an in-flight handshake are still live. */
        mbedtls_platform_set_calloc_free(bb_mem_arena_tls_calloc_impl,
                                          bb_mem_arena_tls_free_impl);
    }
#else
    if (!s_installed) {
        s_installed = true;
        mbedtls_platform_set_calloc_free(bb_mem_arena_tls_calloc_impl,
                                          bb_mem_arena_tls_free_impl);
    }
#endif
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

/* Optional EARLY-tier auto-register (convenience; explicit call is the safe contract). */
#if defined(CONFIG_BB_MEM_ARENA_TLS_AUTOREGISTER)
#include "bb_init.h"
static bb_err_t bb_mem_arena_tls_early_init(void)
{
    bb_mem_arena_tls_init();
    return BB_OK;
}
BB_INIT_REGISTER_EARLY(bb_mem_arena_tls, bb_mem_arena_tls_early_init)
#endif

#else /* !CONFIG_MBEDTLS_CUSTOM_MEM_ALLOC */

/* CUSTOM_MEM_ALLOC not set: esp_mem.c allocator stands; install nothing. */
void bb_mem_arena_tls_init(void) { }
bool bb_mem_arena_tls_owns(const void *ptr) { (void)ptr; return false; }

#endif /* CONFIG_MBEDTLS_CUSTOM_MEM_ALLOC */
