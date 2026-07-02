#pragma once
#include <stddef.h>
#include <stdbool.h>
#include "bb_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * bb_arena — generic, multi-instance contiguous-buffer memory arena.
 *
 * The arena struct lives at the head of the backing buffer (carved off at
 * init time); the remaining bytes form a max_align_t-aligned bump-allocatable
 * data region. Single-threaded: bb_arena_free() does not reclaim space (it
 * only updates the free_count statistic); bb_arena_reset() rewinds the
 * offset so the whole region can be reused at once. Multiple independent
 * instances may coexist — each bb_arena_t is self-contained.
 *
 * Three ways to obtain a handle:
 *   - bb_arena_init()        — caller-supplied buffer (e.g. static BSS).
 *                               No heap allocation; bb_arena_destroy() is a
 *                               no-op (the caller owns the buffer's lifetime).
 *   - bb_arena_init_heap()   — one contiguous allocation via bb_mem
 *                               (components/bb_core/include/bb_mem.h):
 *                               SPIRAM-preferred with internal-heap fallback
 *                               on ESP-IDF, plain calloc on host. Freed by
 *                               bb_arena_destroy().
 *   - bb_arena_init_spiram() — same bb_mem-backed allocation as
 *                               bb_arena_init_heap(); the distinct name
 *                               documents caller intent to keep the region
 *                               off internal RAM. Also freed by
 *                               bb_arena_destroy().
 *
 * bb_arena_init_heap/_spiram intentionally route through the shared bb_mem
 * SPIRAM-preferred facade rather than hand-rolling a second allocator —
 * see the breadboard CLAUDE.md "reuse-or-extract shared helpers" rule.
 *
 * Lifetime: a caller-supplied-buffer arena is valid as long as the backing
 * buffer is live. Do NOT pass a stack-allocated buffer unless the arena
 * does not outlive the frame. After bb_arena_destroy() returns, the handle
 * is invalid for all backing types and must not be reused, re-destroyed, or
 * dereferenced.
 */
typedef struct bb_arena *bb_arena_t;

typedef struct {
    size_t alloc_count;   /**< Successful allocations */
    size_t free_count;    /**< Frees routed through bb_arena_free */
    size_t alloc_failed;  /**< Allocations that returned NULL */
} bb_arena_stats_t;

/**
 * Initialize an arena on a caller-supplied buffer. The arena struct is
 * carved from the front of buf; the remaining bytes are the allocatable
 * region. buf must be aligned to at least _Alignof(max_align_t). Minimum
 * useful size is sizeof(struct bb_arena) rounded up to that alignment, plus
 * one more alignment unit.
 * Returns BB_ERR_INVALID_ARG if out/buf is NULL or size is too small.
 */
bb_err_t bb_arena_init(bb_arena_t *out, void *buf, size_t size);

/**
 * Allocate a `size`-byte backing buffer via bb_mem (SPIRAM-preferred with
 * internal fallback on ESP-IDF; plain calloc on host), then initialize an
 * arena on it. Freed by bb_arena_destroy().
 * Returns BB_ERR_INVALID_ARG if out is NULL or size is 0, BB_ERR_NO_MEM on
 * allocation failure.
 */
bb_err_t bb_arena_init_heap(bb_arena_t *out, size_t size);

/**
 * SPIRAM-preferred variant of bb_arena_init_heap() — same underlying bb_mem
 * allocation; the distinct name documents caller intent to keep the region
 * off internal RAM. Freed by bb_arena_destroy().
 */
bb_err_t bb_arena_init_spiram(bb_arena_t *out, size_t size);

/**
 * Allocate bytes from the arena (bump allocator). Every returned pointer is
 * aligned to _Alignof(max_align_t), satisfying worst-case alignment for any
 * object type (including structs with the strictest fundamental alignment
 * requirement) — safe to back pools of arbitrary objects.
 * Returns NULL on exhaustion, when bytes==0, or when a is NULL.
 */
void *bb_arena_alloc(bb_arena_t a, size_t bytes);

/**
 * Free a pointer previously returned by bb_arena_alloc.
 * The pointer MUST be owned by a (bb_arena_owns returns true) or NULL.
 * The bump allocator does not reclaim space; free_count is still updated.
 */
void bb_arena_free(bb_arena_t a, void *ptr);

/**
 * Reset the arena — logically frees every live pointer. Subsequent
 * allocations reuse the same space from the beginning. Stats counters are
 * preserved. No-op for NULL arena.
 */
void bb_arena_reset(bb_arena_t a);

/**
 * Returns true if ptr falls within the arena's data region. O(1).
 * Returns false for NULL ptr or NULL arena.
 */
bool bb_arena_owns(bb_arena_t a, const void *ptr);

/**
 * Returns free space remaining (arena size minus current bump offset).
 * Returns 0 for NULL arena.
 */
size_t bb_arena_free_bytes(bb_arena_t a);

/** Copy a stats snapshot into *out. No-op for NULL arena or NULL out. */
void bb_arena_get_stats(bb_arena_t a, bb_arena_stats_t *out);

/**
 * Destroy an arena. For a caller-supplied buffer (bb_arena_init), this is a
 * no-op — the caller owns the buffer's lifetime. For a bb_mem-backed arena
 * (bb_arena_init_heap / bb_arena_init_spiram), frees the underlying
 * allocation via bb_mem_free. No-op for NULL arena.
 */
void bb_arena_destroy(bb_arena_t a);

#ifdef __cplusplus
}
#endif
