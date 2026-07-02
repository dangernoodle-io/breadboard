/* bb_arena — generic contiguous-buffer bump allocator.
 *
 * Shared implementation: compiled for both host (native_scaffold.py) and
 * ESP-IDF (components/bb_arena/CMakeLists.txt) builds. The bump/owns/reset/
 * stats logic is fully portable (no ESP-IDF headers). The bb_mem facade
 * (components/bb_core/include/bb_mem.h) already abstracts the
 * SPIRAM-preferred-with-internal-fallback (ESP-IDF) vs plain calloc (host)
 * split, so bb_arena_init_heap/_spiram reuse it directly rather than
 * hand-rolling a second per-platform allocator.
 *
 * The arena struct lives at the head of the backing buffer; the remaining
 * bytes form the bump-allocatable data region. Single-threaded: free() does
 * not reclaim space (bump allocator) but updates the free_count statistic;
 * reset() rewinds the bump offset so the space can be reused.
 */
#include "bb_arena.h"
#include "bb_mem.h"
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Round struct size up to max_align_t alignment so allocations satisfy
 * worst-case alignment for arbitrary object types (see bb_arena_alloc doc). */
#define BB_ARENA_ALIGN (_Alignof(max_align_t))
#define BB_ARENA_HDR_SZ \
    ((sizeof(struct bb_arena) + (BB_ARENA_ALIGN - 1u)) & ~(BB_ARENA_ALIGN - 1u))

struct bb_arena {
    uint8_t          *buf;         /* start of data region */
    size_t            size;        /* byte count of data region */
    size_t            offset;      /* current bump offset within data region */
    bb_arena_stats_t  stats;
    void             *owned_block; /* NULL for caller-supplied buffers;
                                     * non-NULL block is freed via bb_mem_free
                                     * by bb_arena_destroy(). */
};

static bb_err_t bb_arena_carve(bb_arena_t *out, void *block, size_t block_size)
{
    if (!out || !block || block_size < BB_ARENA_HDR_SZ + BB_ARENA_ALIGN) {
        return BB_ERR_INVALID_ARG;
    }
    struct bb_arena *a = (struct bb_arena *)block;
    memset(a, 0, sizeof(*a));
    a->buf  = (uint8_t *)block + BB_ARENA_HDR_SZ;
    a->size = block_size - BB_ARENA_HDR_SZ;
    *out = a;
    return BB_OK;
}

bb_err_t bb_arena_init(bb_arena_t *out, void *buf, size_t size)
{
    return bb_arena_carve(out, buf, size);
}

static bb_err_t bb_arena_init_from_bb_mem(bb_arena_t *out, size_t size)
{
    if (!out || size == 0) {
        return BB_ERR_INVALID_ARG;
    }
    if (size > SIZE_MAX - BB_ARENA_HDR_SZ) {
        return BB_ERR_INVALID_ARG;
    }
    size_t total = BB_ARENA_HDR_SZ + size;
    void *block = bb_calloc_prefer_spiram(1, total);
    if (!block) {
        return BB_ERR_NO_MEM;
    }
    bb_err_t err = bb_arena_carve(out, block, total);
    if (err != BB_OK) {
        bb_mem_free(block);
        return err;
    }
    (*out)->owned_block = block;
    return BB_OK;
}

bb_err_t bb_arena_init_heap(bb_arena_t *out, size_t size)
{
    return bb_arena_init_from_bb_mem(out, size);
}

bb_err_t bb_arena_init_spiram(bb_arena_t *out, size_t size)
{
    return bb_arena_init_from_bb_mem(out, size);
}

void *bb_arena_alloc(bb_arena_t a, size_t bytes)
{
    if (!a || bytes == 0) {
        if (a) {
            a->stats.alloc_failed++;
        }
        return NULL;
    }
    /* Reject before rounding/summing so a huge `bytes` near SIZE_MAX cannot
     * wrap either the alignment round-up or the offset+aligned bounds check.
     * a->offset <= a->size always holds, so this subtraction cannot
     * underflow. */
    size_t remaining = a->size - a->offset;
    if (bytes > remaining) {
        a->stats.alloc_failed++;
        return NULL;
    }
    /* Round up to BB_ARENA_ALIGN-byte alignment. */
    size_t aligned = (bytes + (BB_ARENA_ALIGN - 1u)) & ~(BB_ARENA_ALIGN - 1u);
    /* defensive: unreachable via public API given arena size bounds; guards
     * against future callers. */
    if (aligned < bytes || aligned > remaining) {
        a->stats.alloc_failed++;
        return NULL;
    }
    void *p = a->buf + a->offset;
    a->offset += aligned;
    a->stats.alloc_count++;
    return p;
}

void bb_arena_free(bb_arena_t a, void *ptr)
{
    if (!a || !ptr) {
        return;
    }
    assert(bb_arena_owns(a, ptr));
    /* Bump allocator: reclaim is not supported; track the free for stats. */
    a->stats.free_count++;
}

void bb_arena_reset(bb_arena_t a)
{
    if (!a) {
        return;
    }
    a->offset = 0;
}

bool bb_arena_owns(bb_arena_t a, const void *ptr)
{
    if (!a || !ptr) {
        return false;
    }
    return (ptr >= (const void *)a->buf &&
            ptr <  (const void *)(a->buf + a->size));
}

size_t bb_arena_free_bytes(bb_arena_t a)
{
    if (!a) {
        return 0;
    }
    return a->size - a->offset;
}

void bb_arena_get_stats(bb_arena_t a, bb_arena_stats_t *out)
{
    if (!a || !out) {
        return;
    }
    *out = a->stats;
}

void bb_arena_destroy(bb_arena_t a)
{
    if (!a) {
        return;
    }
    /* Contract: after destroy() the handle is invalid and must not be
     * re-destroyed or reused. For caller-supplied-buffer arenas (owned_block
     * NULL) nothing is freed, so re-destroy of that path happens to be safe.
     * For bb_mem-backed arenas the bb_arena struct itself lives inside the
     * freed block, so re-destroy dereferences freed memory — undefined
     * behavior (a real UAF/double-free under CONFIG_HEAP_POISONING/ASAN).
     * Callers should null their own handle immediately after calling this. */
    if (a->owned_block) {
        void *block = a->owned_block;
        a->owned_block = NULL;
        bb_mem_free(block);
    }
}
