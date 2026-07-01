#include "bb_heap_arena.h"
#include "bb_mem.h"
#include <string.h>
#include <stdint.h>
#include <stddef.h>

#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif

/* Guard: if arena bytes > 0 but MBEDTLS_CUSTOM_MEM_ALLOC is not set, the
 * static arena buffer is never registered as an allocator — all those bytes
 * sit unused in BSS forever.  Catch this misconfiguration at compile time. */
#if defined(CONFIG_BB_HEAP_ARENA_BYTES) && (CONFIG_BB_HEAP_ARENA_BYTES > 0) && \
    !defined(CONFIG_MBEDTLS_CUSTOM_MEM_ALLOC)
#  error "BB_HEAP_ARENA_BYTES > 0 requires CONFIG_MBEDTLS_CUSTOM_MEM_ALLOC=y " \
         "(arena allocated but mbedTLS will never use it — wasted BSS); " \
         "set CONFIG_MBEDTLS_CUSTOM_MEM_ALLOC=y or set CONFIG_BB_HEAP_ARENA_BYTES=0"
#endif

/* Only install when CONFIG_MBEDTLS_CUSTOM_MEM_ALLOC is set.
 * When NOT set, esp_mem.c (ESP-IDF) handles the allocator; we install nothing. */
#if defined(CONFIG_MBEDTLS_CUSTOM_MEM_ALLOC)

#include "mbedtls/platform.h"
#include "esp_heap_caps.h"
#include "multi_heap.h"

#if defined(CONFIG_BB_HEAP_ARENA_BYTES) && CONFIG_BB_HEAP_ARENA_BYTES > 0
/* Static arena buffer — aligned to pointer size for multi_heap requirements. */
static uint8_t s_arena[CONFIG_BB_HEAP_ARENA_BYTES] __attribute__((aligned(8)));
static multi_heap_handle_t s_heap = NULL;
#define ARENA_ENABLED 1
#else
#define ARENA_ENABLED 0
#endif

static const char *TAG = "bb_heap_arena";

static void *bb_heap_arena_calloc_impl(size_t n, size_t size)
{
    size_t total = n * size;
    if (total == 0) return NULL;

#if ARENA_ENABLED
    if (s_heap != NULL) {
        void *p = multi_heap_malloc(s_heap, total);
        if (p) {
            memset(p, 0, total);
            return p;
        }
    }
#endif
    /* Fallback: internal 8-bit heap (matches ESP-IDF default in esp_mem.c) */
    return bb_calloc_internal(n, size);
}

static void bb_heap_arena_free_impl(void *ptr)
{
    if (!ptr) return;

#if ARENA_ENABLED
    if ((void *)ptr >= (void *)s_arena &&
        (void *)ptr < (void *)(s_arena + sizeof(s_arena))) {
        multi_heap_free(s_heap, ptr);
        return;
    }
#endif
    bb_mem_free(ptr);
}

void bb_heap_arena_init(void)
{
#if ARENA_ENABLED
    if (s_heap == NULL) {
        s_heap = multi_heap_register(s_arena, sizeof(s_arena));
    }
#endif
    /* Install custom allocator (required when CUSTOM_MEM_ALLOC=y — ESP-IDF
     * compiles out esp_mem.c, leaving no default; we MUST install one). */
    mbedtls_platform_set_calloc_free(bb_heap_arena_calloc_impl,
                                     bb_heap_arena_free_impl);
}

bool bb_heap_arena_owns(const void *ptr)
{
#if ARENA_ENABLED
    return (ptr >= (const void *)s_arena &&
            ptr < (const void *)(s_arena + sizeof(s_arena)));
#else
    (void)ptr;
    return false;
#endif
}

/* Optional EARLY-tier auto-register (convenience; explicit call is the safe contract). */
#if defined(CONFIG_BB_HEAP_ARENA_AUTOREGISTER)
#include "bb_registry.h"
static bb_err_t bb_heap_arena_early_init(void)
{
    bb_heap_arena_init();
    return BB_OK;
}
BB_REGISTRY_REGISTER_EARLY(bb_heap_arena, bb_heap_arena_early_init)
#endif

#else /* !CONFIG_MBEDTLS_CUSTOM_MEM_ALLOC */

/* CUSTOM_MEM_ALLOC not set: esp_mem.c allocator stands; install nothing. */
void bb_heap_arena_init(void) { }
bool bb_heap_arena_owns(const void *ptr) { (void)ptr; return false; }

#endif /* CONFIG_MBEDTLS_CUSTOM_MEM_ALLOC */
