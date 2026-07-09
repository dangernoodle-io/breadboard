#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "bb_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * bb_mem_arena_tls — boot-reserved contiguous mbedTLS handshake arena.
 *
 * Consumer of the generic bb_mem_arena primitive (components/bb_mem_arena): the
 * arena mechanics (bump-alloc, owns, reset, stats) live in bb_mem_arena; this
 * component wires them into mbedTLS's custom-allocator hook and layers a
 * fallback to the internal-heap facade (bb_mem's bb_calloc_internal /
 * bb_mem_free) on arena exhaustion.
 *
 * ORDERING CONTRACT (CRITICAL):
 *   Call bb_mem_arena_tls_init() at the very top of app_main(), BEFORE
 *   bb_init_init_early(). The mbedTLS allocator must be installed before
 *   any WiFi/WPA-supplicant mbedTLS use (which occurs during esp_wifi_connect
 *   inside the EARLY-tier bb_wifi init). Calling this function after
 *   bb_init_init_early() is undefined behavior.
 *
 * When CONFIG_MBEDTLS_CUSTOM_MEM_ALLOC is NOT set, this function is a no-op
 * and ESP-IDF's default mbedTLS allocator (esp_mem.c) handles all allocations.
 *
 * When CONFIG_MBEDTLS_CUSTOM_MEM_ALLOC IS set:
 *   - Installs bb_mem_arena_tls_calloc/bb_mem_arena_tls_free as the global mbedTLS
 *     allocator via mbedtls_platform_set_calloc_free().
 *   - If CONFIG_BB_MEM_ARENA_TLS_BYTES > 0: reserves a static contiguous buffer
 *     of that size (via bb_mem_arena_init on a static BSS buffer) and serves
 *     allocations arena-first (try-arena, fall back to
 *     bb_calloc_internal()/bb_mem_free() when exhausted).
 *   - If CONFIG_BB_MEM_ARENA_TLS_BYTES == 0: no arena buffer; calloc/free route
 *     directly to bb_calloc_internal() / bb_mem_free().
 *
 * CONCURRENCY: the installed calloc/free hooks are the process-wide mbedTLS
 * allocator and are internally mutex-guarded, so they are safe to call from
 * multiple concurrent TLS handshakes (e.g. telemetry sink + OTA/update-check,
 * WPA-supplicant). Concurrent callers are serialized against the shared
 * arena/counter, not given independent arenas — size CONFIG_BB_MEM_ARENA_TLS_BYTES
 * for cumulative concurrent usage (see Kconfig help).
 *
 * bb_mem_arena_tls_init() is idempotent: a second call (e.g. an explicit
 * app_main() call plus the codegen'd EARLY-tier init) is a true no-op and
 * will not zero the outstanding-allocation counter or rewind the arena while
 * allocations from an in-flight handshake are still live.
 *
 * REUSE ACROSS HANDSHAKES: the underlying bb_mem_arena is a bump allocator that
 * does not reclaim on individual frees. This component tracks outstanding
 * arena allocations and calls bb_mem_arena_reset() only once that count drains
 * to zero (i.e. every arena allocation from the handshake has been freed),
 * rewinding the bump pointer so the next handshake can reuse the whole
 * region. Size CONFIG_BB_MEM_ARENA_TLS_BYTES for the cumulative allocation of a
 * single handshake, not just its peak concurrently-live bytes — see the
 * Kconfig help text.
 */
void bb_mem_arena_tls_init(void);

/**
 * Returns true if ptr falls within the arena buffer.
 * Returns false if no arena is configured (BYTES == 0) or CUSTOM_MEM_ALLOC is off.
 * Host-testable pure predicate.
 */
bool bb_mem_arena_tls_owns(const void *ptr);

/**
 * Registry hook — calls bb_mem_arena_tls_init(). See the ORDERING CONTRACT
 * above: the safe contract is an explicit call at the top of app_main(),
 * before any WiFi/WPA-supplicant mbedTLS use. No-op when
 * CONFIG_MBEDTLS_CUSTOM_MEM_ALLOC is off.
 */
// bbtool:init tier=early fn=bb_mem_arena_tls_early_init
bb_err_t bb_mem_arena_tls_early_init(void);

#if defined(BB_MEM_ARENA_TLS_TESTING)
/* Test hooks: exercise routing without going through mbedtls_platform. */
void *bb_mem_arena_tls_calloc(size_t n, size_t size);
void  bb_mem_arena_tls_free(void *ptr);
/* Reset arena state (re-run init from scratch) for test isolation. */
void  bb_mem_arena_tls_test_reset(void);
#endif

#ifdef __cplusplus
}
#endif
