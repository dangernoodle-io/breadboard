// bb_pool — generic object/memory pool carved from a bb_arena.
//
// Compiled on both host (tests) and ESP-IDF. All pool storage is carved from
// the backing bb_arena via bb_arena_alloc; bb_pool never calls the global
// heap allocator directly (bb_pool_create_owned routes through bb_arena's
// bb_mem-backed init helpers, same as any other arena-owning consumer).

#include "bb_pool.h"
#include "bb_arena.h"
#include "bb_log.h"

#include <string.h>
#include <stdint.h>

static const char *TAG = "bb_pool";

// Max name length stored in the pool struct (diagnostic only).
#define BB_POOL_NAME_MAX 24

// Alignment used for every carved allocation — matches bb_arena_alloc's
// guarantee (max_align_t), so slots/entries returned to callers satisfy the
// worst-case alignment for any object type.
#define BB_POOL_ALIGN ((size_t)_Alignof(max_align_t))

// Overflow-safe: returns SIZE_MAX (an unreachable arena size in practice)
// rather than wrapping when n is within BB_POOL_ALIGN-1 of SIZE_MAX. Callers
// that feed the result into bb_pool_arena_size_needed's add_ovf/mul_ovf chain
// will see the SIZE_MAX propagate into an add_ovf failure -> 0 return, per
// that function's documented overflow contract.
static size_t align_up(size_t n)
{
    if (n > SIZE_MAX - (BB_POOL_ALIGN - 1u)) return SIZE_MAX;
    return (n + BB_POOL_ALIGN - 1u) & ~(BB_POOL_ALIGN - 1u);
}

// ---------------------------------------------------------------------------
// Overflow-safe arithmetic helpers
// ---------------------------------------------------------------------------

static bool add_ovf(size_t a, size_t b, size_t *out)
{
    if (a > SIZE_MAX - b) return true;
    *out = a + b;
    return false;
}

static bool mul_ovf(size_t a, size_t b, size_t *out)
{
    if (a != 0 && b > SIZE_MAX / a) return true;
    *out = a * b;
    return false;
}

// ---------------------------------------------------------------------------
// RETAINED path storage
// ---------------------------------------------------------------------------

typedef struct {
    uint8_t *data;   /* arena-allocated buffer, max_slot_bytes bytes */
    size_t   len;    /* current content length; 0 before first update */
    bool     valid;  /* true after first retained_update */
} bb_pool_retained_slot_t;

// ---------------------------------------------------------------------------
// FIFO path storage — internal arena-carved ring (no bb_ring dependency)
// ---------------------------------------------------------------------------

typedef struct {
    size_t   len;
    int64_t  ts;
    uint32_t id;
} bb_pool_fifo_hdr_t;

typedef struct {
    uint8_t *entries;       /* capacity * entry_stride bytes */
    size_t   entry_stride;  /* aligned sizeof(hdr) + max_slot_bytes */
    size_t   head;          /* logical index of oldest entry */
    size_t   count;
    size_t   dropped;
} bb_pool_fifo_t;

// ---------------------------------------------------------------------------
// SLOTS path storage — free-list over fixed-size slots
// ---------------------------------------------------------------------------

typedef struct {
    uint8_t *storage;      /* capacity * slot_stride bytes */
    size_t   slot_stride;  /* aligned max_slot_bytes */
    void   **free_list;    /* capacity pointers; stack (LIFO) */
    size_t   free_top;     /* number of free entries currently on the stack */
    uint8_t *acquired_bitmap; /* ceil(capacity/8) bytes; bit set iff slot idx
                               * is currently out on loan (acquired and not
                               * yet released). Detects double-release. */
} bb_pool_slots_t;

// ---------------------------------------------------------------------------
// SLOTS bitmap helpers — one bit per slot, set iff currently acquired.
// ---------------------------------------------------------------------------

static size_t bitmap_bytes_for(size_t capacity)
{
    size_t bits_rounded;
    if (add_ovf(capacity, 7u, &bits_rounded)) return SIZE_MAX;
    return bits_rounded / 8u;
}

static bool bitmap_test(const uint8_t *bm, size_t idx)
{
    return (bm[idx / 8u] & (uint8_t)(1u << (idx % 8u))) != 0;
}

static void bitmap_set(uint8_t *bm, size_t idx)
{
    bm[idx / 8u] |= (uint8_t)(1u << (idx % 8u));
}

static void bitmap_clear(uint8_t *bm, size_t idx)
{
    bm[idx / 8u] &= (uint8_t)~(1u << (idx % 8u));
}

// ---------------------------------------------------------------------------
// Pool struct — allocated from the backing arena at create time (except the
// bb_pool_create_owned() wrapper struct fields set post-hoc; the struct
// itself always lives in the arena).
// ---------------------------------------------------------------------------

struct bb_pool {
    bb_pool_cfg_t cfg;
    bb_arena_t    arena;       /* backing arena; owned iff owns_arena */
    bool          owns_arena;
    char          name[BB_POOL_NAME_MAX];

    bb_pool_retained_slot_t *retained_slots; /* RETAINED mode */
    bb_pool_fifo_t           fifo;           /* FIFO mode */
    bb_pool_slots_t          slots;          /* SLOTS mode */
};

// ---------------------------------------------------------------------------
// Sizing helper
// ---------------------------------------------------------------------------

size_t bb_pool_arena_size_needed(const bb_pool_cfg_t *cfg)
{
    if (!cfg) return 0;

    size_t total = align_up(sizeof(struct bb_pool));

    switch (cfg->mode) {
    case BB_POOL_MODE_TRANSIENT: {
        /* No fixed structure beyond the pool struct itself, plus the
         * caller-requested bump-space reserve (owned-arena sizing only;
         * ignored/harmless for bb_pool_create() against a caller-supplied
         * arena that is already sized some other way). */
        if (cfg->transient_reserve_bytes == 0) return total;
        if (add_ovf(total, cfg->transient_reserve_bytes, &total)) return 0;
        return total;
    }

    case BB_POOL_MODE_RETAINED: {
        if (!cfg->capacity || !cfg->max_slot_bytes) return 0;

        size_t slot_array_bytes;
        if (mul_ovf(cfg->capacity, sizeof(bb_pool_retained_slot_t), &slot_array_bytes)) return 0;
        slot_array_bytes = align_up(slot_array_bytes);
        if (add_ovf(total, slot_array_bytes, &total)) return 0;

        size_t slot_stride = align_up(cfg->max_slot_bytes);
        size_t data_bytes;
        if (mul_ovf(cfg->capacity, slot_stride, &data_bytes)) return 0;
        if (add_ovf(total, data_bytes, &total)) return 0;

        return total;
    }

    case BB_POOL_MODE_FIFO: {
        if (!cfg->capacity || !cfg->max_slot_bytes) return 0;

        size_t hdr_payload;
        if (add_ovf(sizeof(bb_pool_fifo_hdr_t), cfg->max_slot_bytes, &hdr_payload)) return 0;
        size_t entry_stride = align_up(hdr_payload);

        size_t ring_bytes;
        if (mul_ovf(cfg->capacity, entry_stride, &ring_bytes)) return 0;
        if (add_ovf(total, ring_bytes, &total)) return 0;

        return total;
    }

    case BB_POOL_MODE_SLOTS: {
        if (!cfg->capacity || !cfg->max_slot_bytes) return 0;

        size_t slot_stride = align_up(cfg->max_slot_bytes);
        size_t storage_bytes;
        if (mul_ovf(cfg->capacity, slot_stride, &storage_bytes)) return 0;
        if (add_ovf(total, storage_bytes, &total)) return 0;

        size_t free_list_bytes;
        if (mul_ovf(cfg->capacity, sizeof(void *), &free_list_bytes)) return 0;
        free_list_bytes = align_up(free_list_bytes);
        if (add_ovf(total, free_list_bytes, &total)) return 0;

        size_t bitmap_bytes = bitmap_bytes_for(cfg->capacity);
        if (bitmap_bytes == SIZE_MAX) return 0;
        bitmap_bytes = align_up(bitmap_bytes);
        if (add_ovf(total, bitmap_bytes, &total)) return 0;

        return total;
    }

    default:
        return 0;
    }
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

bb_err_t bb_pool_create(const bb_pool_cfg_t *cfg, bb_arena_t arena,
                         bb_pool_t *out)
{
    if (!cfg || !arena || !out) return BB_ERR_INVALID_ARG;

    switch (cfg->mode) {
    case BB_POOL_MODE_RETAINED:
    case BB_POOL_MODE_FIFO:
    case BB_POOL_MODE_SLOTS:
        if (!cfg->capacity || !cfg->max_slot_bytes) return BB_ERR_INVALID_ARG;
        break;
    case BB_POOL_MODE_TRANSIENT:
        break;
    default:
        return BB_ERR_INVALID_ARG;
    }

    // Route through the same overflow-checked sizing path used by
    // bb_pool_create_owned() / bb_pool_arena_size_needed() rather than
    // re-deriving carve sizes with raw multiplications below — a 0 return
    // means either an invalid cfg or size_t overflow on this capacity /
    // max_slot_bytes combination. Every multiplication further down in this
    // function operates on the same cfg fields already proven not to
    // overflow by this call.
    size_t need = bb_pool_arena_size_needed(cfg);
    if (!need) return BB_ERR_INVALID_ARG;
    if (bb_arena_free_bytes(arena) < need) {
        bb_log_e(TAG, "create: arena has %zu B free, need %zu B", bb_arena_free_bytes(arena), need);
        return BB_ERR_NO_SPACE;
    }

    struct bb_pool *p = (struct bb_pool *)bb_arena_alloc(arena, sizeof(struct bb_pool));
    if (!p) {
        bb_log_e(TAG, "create: arena exhausted allocating pool struct");
        return BB_ERR_NO_SPACE;
    }
    memset(p, 0, sizeof(*p));
    p->cfg   = *cfg;
    p->arena = arena;
    p->owns_arena = false;

    if (cfg->name) {
        strncpy(p->name, cfg->name, BB_POOL_NAME_MAX - 1);
        p->name[BB_POOL_NAME_MAX - 1] = '\0';
    }

    if (cfg->mode == BB_POOL_MODE_RETAINED) {
        p->retained_slots = (bb_pool_retained_slot_t *)bb_arena_alloc(
                                 arena, cfg->capacity * sizeof(bb_pool_retained_slot_t));
        if (!p->retained_slots) {
            bb_log_e(TAG, "'%s': arena exhausted allocating retained slot array", p->name);
            return BB_ERR_NO_SPACE;
        }
        memset(p->retained_slots, 0, cfg->capacity * sizeof(bb_pool_retained_slot_t));

        for (size_t i = 0; i < cfg->capacity; i++) {
            p->retained_slots[i].data = (uint8_t *)bb_arena_alloc(arena, cfg->max_slot_bytes);
            if (!p->retained_slots[i].data) {
                bb_log_e(TAG, "'%s': arena exhausted at retained slot %zu", p->name, i);
                return BB_ERR_NO_SPACE;
            }
        }
    } else if (cfg->mode == BB_POOL_MODE_FIFO) {
        size_t entry_stride = align_up(sizeof(bb_pool_fifo_hdr_t) + cfg->max_slot_bytes);
        size_t ring_bytes = cfg->capacity * entry_stride;

        p->fifo.entries = (uint8_t *)bb_arena_alloc(arena, ring_bytes);
        if (!p->fifo.entries) {
            bb_log_e(TAG, "'%s': arena exhausted allocating FIFO ring (%zu B)", p->name, ring_bytes);
            return BB_ERR_NO_SPACE;
        }
        p->fifo.entry_stride = entry_stride;
        p->fifo.head = 0;
        p->fifo.count = 0;
        p->fifo.dropped = 0;
    } else if (cfg->mode == BB_POOL_MODE_SLOTS) {
        size_t slot_stride = align_up(cfg->max_slot_bytes);
        size_t storage_bytes = cfg->capacity * slot_stride;

        p->slots.storage = (uint8_t *)bb_arena_alloc(arena, storage_bytes);
        if (!p->slots.storage) {
            bb_log_e(TAG, "'%s': arena exhausted allocating slot storage (%zu B)", p->name, storage_bytes);
            return BB_ERR_NO_SPACE;
        }
        p->slots.free_list = (void **)bb_arena_alloc(arena, cfg->capacity * sizeof(void *));
        if (!p->slots.free_list) {
            bb_log_e(TAG, "'%s': arena exhausted allocating slot free-list", p->name);
            return BB_ERR_NO_SPACE;
        }

        size_t bitmap_bytes = align_up(bitmap_bytes_for(cfg->capacity));
        p->slots.acquired_bitmap = (uint8_t *)bb_arena_alloc(arena, bitmap_bytes);
        if (!p->slots.acquired_bitmap) {
            bb_log_e(TAG, "'%s': arena exhausted allocating slot bitmap", p->name);
            return BB_ERR_NO_SPACE;
        }
        memset(p->slots.acquired_bitmap, 0, bitmap_bytes); /* all slots start free */

        p->slots.slot_stride = slot_stride;
        for (size_t i = 0; i < cfg->capacity; i++) {
            p->slots.free_list[i] = p->slots.storage + i * slot_stride;
        }
        p->slots.free_top = cfg->capacity;
    }

    bb_log_i(TAG, "created pool '%s': mode=%d cap=%zu slot=%zuB",
             p->name, (int)cfg->mode, cfg->capacity, cfg->max_slot_bytes);

    *out = p;
    return BB_OK;
}

bb_err_t bb_pool_create_owned(const bb_pool_cfg_t *cfg,
                               bb_pool_backing_t backing, bb_pool_t *out)
{
    if (!cfg || !out) return BB_ERR_INVALID_ARG;

    size_t need = bb_pool_arena_size_needed(cfg);
    if (!need) return BB_ERR_INVALID_ARG;

    bb_arena_t arena;
    bb_err_t rc = (backing == BB_POOL_BACKING_SPIRAM)
                      ? bb_arena_init_spiram(&arena, need)
                      : bb_arena_init_heap(&arena, need);
    if (rc != BB_OK) return rc;

    bb_pool_t p;
    rc = bb_pool_create(cfg, arena, &p);
    if (rc != BB_OK) {
        bb_arena_destroy(arena);
        return rc;
    }

    p->owns_arena = true;
    *out = p;
    return BB_OK;
}

void bb_pool_destroy(bb_pool_t pool)
{
    if (!pool) return;
    if (pool->owns_arena) {
        /* Frees the single bb_mem allocation backing the arena, which also
         * contains the pool struct and all mode-specific storage — pool
         * must not be touched after this call. */
        bb_arena_destroy(pool->arena);
        return;
    }
    bb_log_d(TAG, "destroyed pool '%s' (caller-owned arena)", pool->name);
}

void bb_pool_get_stats(bb_pool_t pool, bb_arena_stats_t *out)
{
    if (!pool || !out) return;
    bb_arena_get_stats(pool->arena, out);
}

// ---------------------------------------------------------------------------
// TRANSIENT path
// ---------------------------------------------------------------------------

void *bb_pool_alloc(bb_pool_t pool, size_t bytes)
{
    if (!pool) return NULL;
    return bb_arena_alloc(pool->arena, bytes);
}

void bb_pool_free(bb_pool_t pool, void *ptr)
{
    if (!pool) return;
    bb_arena_free(pool->arena, ptr);
}

void bb_pool_reset(bb_pool_t pool)
{
    if (!pool) return;
    bb_arena_reset(pool->arena);
}

// ---------------------------------------------------------------------------
// RETAINED path
// ---------------------------------------------------------------------------

bb_err_t bb_pool_retained_update(bb_pool_t pool, size_t slot_idx,
                                  const void *data, size_t len)
{
    if (!pool) return BB_ERR_INVALID_ARG;
    if (pool->cfg.mode != BB_POOL_MODE_RETAINED) return BB_ERR_INVALID_STATE;
    if (slot_idx >= pool->cfg.capacity) return BB_ERR_INVALID_ARG;
    if (len > pool->cfg.max_slot_bytes) return BB_ERR_INVALID_ARG;
    if (!data && len > 0) return BB_ERR_INVALID_ARG;

    bb_pool_retained_slot_t *s = &pool->retained_slots[slot_idx];
    if (len > 0) memcpy(s->data, data, len);
    s->len   = len;
    s->valid = true;
    return BB_OK;
}

bb_err_t bb_pool_retained_get(bb_pool_t pool, size_t slot_idx,
                               const void **out_ptr, size_t *out_len)
{
    if (!pool || !out_ptr || !out_len) return BB_ERR_INVALID_ARG;
    if (pool->cfg.mode != BB_POOL_MODE_RETAINED) return BB_ERR_INVALID_STATE;
    if (slot_idx >= pool->cfg.capacity) return BB_ERR_INVALID_ARG;

    bb_pool_retained_slot_t *s = &pool->retained_slots[slot_idx];
    if (!s->valid) return BB_ERR_NOT_FOUND;

    *out_ptr = s->data;
    *out_len = s->len;
    return BB_OK;
}

// ---------------------------------------------------------------------------
// FIFO path — internal arena-carved ring
// ---------------------------------------------------------------------------

static uint8_t *fifo_entry_at(struct bb_pool *pool, size_t logical_idx)
{
    size_t phys = (pool->fifo.head + logical_idx) % pool->cfg.capacity;
    return pool->fifo.entries + phys * pool->fifo.entry_stride;
}

bb_err_t bb_pool_push(bb_pool_t pool, const void *data, size_t len,
                       int64_t ts, uint32_t id)
{
    if (!pool) return BB_ERR_INVALID_ARG;
    if (pool->cfg.mode != BB_POOL_MODE_FIFO) return BB_ERR_INVALID_STATE;
    if (len > pool->cfg.max_slot_bytes) return BB_ERR_INVALID_ARG;
    if (!data && len > 0) return BB_ERR_INVALID_ARG;

    if (pool->fifo.count == pool->cfg.capacity) {
        if (pool->cfg.full_policy == BB_POOL_FULL_REJECT_NEW) {
            pool->fifo.dropped++;
            return BB_ERR_NO_SPACE;
        }
        /* EVICT_OLDEST: drop the oldest entry to make room. */
        pool->fifo.head = (pool->fifo.head + 1) % pool->cfg.capacity;
        pool->fifo.count--;
        pool->fifo.dropped++;
    }

    size_t tail_idx = (pool->fifo.head + pool->fifo.count) % pool->cfg.capacity;
    uint8_t *slot = pool->fifo.entries + tail_idx * pool->fifo.entry_stride;

    bb_pool_fifo_hdr_t hdr;
    memset(&hdr, 0, sizeof(hdr)); /* avoid copying indeterminate struct padding into the arena */
    hdr.len = len;
    hdr.ts  = ts;
    hdr.id  = id;
    memcpy(slot, &hdr, sizeof(hdr));
    if (len > 0) memcpy(slot + sizeof(hdr), data, len);

    pool->fifo.count++;
    return BB_OK;
}

bb_err_t bb_pool_peek_oldest(bb_pool_t pool,
                              void *buf, size_t buf_cap,
                              size_t *out_len,
                              int64_t *out_ts,
                              uint32_t *out_id)
{
    if (!pool || !out_len || !out_ts || !out_id) return BB_ERR_INVALID_ARG;
    if (pool->cfg.mode != BB_POOL_MODE_FIFO) return BB_ERR_INVALID_STATE;
    if (pool->fifo.count == 0) return BB_ERR_NOT_FOUND;

    uint8_t *slot = fifo_entry_at(pool, 0);
    bb_pool_fifo_hdr_t hdr;
    memcpy(&hdr, slot, sizeof(hdr));

    *out_len = hdr.len;
    *out_ts  = hdr.ts;
    *out_id  = hdr.id;

    if (buf && buf_cap > 0 && hdr.len > 0) {
        size_t n = (buf_cap < hdr.len) ? buf_cap : hdr.len;
        memcpy(buf, slot + sizeof(hdr), n);
    }
    return BB_OK;
}

bb_err_t bb_pool_pop_oldest(bb_pool_t pool)
{
    if (!pool) return BB_ERR_INVALID_ARG;
    if (pool->cfg.mode != BB_POOL_MODE_FIFO) return BB_ERR_INVALID_STATE;
    if (pool->fifo.count == 0) return BB_ERR_NOT_FOUND;

    pool->fifo.head = (pool->fifo.head + 1) % pool->cfg.capacity;
    pool->fifo.count--;
    return BB_OK;
}

size_t bb_pool_count(bb_pool_t pool)
{
    if (!pool || pool->cfg.mode != BB_POOL_MODE_FIFO) return 0;
    return pool->fifo.count;
}

size_t bb_pool_dropped(bb_pool_t pool)
{
    if (!pool || pool->cfg.mode != BB_POOL_MODE_FIFO) return 0;
    return pool->fifo.dropped;
}

// ---------------------------------------------------------------------------
// SLOTS path
// ---------------------------------------------------------------------------

// Recover the slot index for a pointer previously handed out by
// bb_pool_acquire, or SIZE_MAX if ptr does not belong to this pool's slot
// storage (out of range, not base-aligned to a slot boundary, or index >=
// capacity).
static size_t slots_index_of(struct bb_pool *pool, const void *ptr)
{
    const uint8_t *base = pool->slots.storage;
    const uint8_t *p = (const uint8_t *)ptr;
    size_t stride = pool->slots.slot_stride;

    if (p < base) return SIZE_MAX;
    size_t offset = (size_t)(p - base);
    if (offset % stride != 0) return SIZE_MAX;
    size_t idx = offset / stride;
    if (idx >= pool->cfg.capacity) return SIZE_MAX;
    return idx;
}

void *bb_pool_acquire(bb_pool_t pool)
{
    if (!pool || pool->cfg.mode != BB_POOL_MODE_SLOTS) return NULL;
    if (pool->slots.free_top == 0) return NULL;

    pool->slots.free_top--;
    void *ptr = pool->slots.free_list[pool->slots.free_top];

    size_t idx = slots_index_of(pool, ptr);
    if (idx != SIZE_MAX) bitmap_set(pool->slots.acquired_bitmap, idx);
    return ptr;
}

bb_err_t bb_pool_release(bb_pool_t pool, void *ptr)
{
    if (!pool || !ptr) return BB_ERR_INVALID_ARG;
    if (pool->cfg.mode != BB_POOL_MODE_SLOTS) return BB_ERR_INVALID_STATE;

    size_t idx = slots_index_of(pool, ptr);
    if (idx == SIZE_MAX) return BB_ERR_INVALID_ARG;

    // Detects double-release (and release of a never-acquired-in-bounds
    // pointer): a slot not currently marked acquired must not be pushed
    // back onto the free-list a second time — that would hand the same
    // memory to two concurrent acquirers.
    if (!bitmap_test(pool->slots.acquired_bitmap, idx)) {
        bb_log_e(TAG, "'%s': double release (or release of unacquired slot) at idx %zu", pool->name, idx);
        return BB_ERR_INVALID_STATE;
    }

    if (pool->slots.free_top >= pool->cfg.capacity) return BB_ERR_INVALID_STATE;
    bitmap_clear(pool->slots.acquired_bitmap, idx);
    pool->slots.free_list[pool->slots.free_top] = ptr;
    pool->slots.free_top++;
    return BB_OK;
}
