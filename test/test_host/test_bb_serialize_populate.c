// bb_serialize_populate -- the pull-based inverse walker. Host-only, no
// hardware dependency (B1-832).
//
// mock_store_t is a fixed-capacity, path-keyed key/value map that plays
// BOTH roles: seeded directly it drives bb_serialize_populate() as a
// bb_serialize_populate_t source; driven as a bb_serialize_emit_t target
// (via bb_serialize_walk()) it RECORDS a golden struct into the same
// path-keyed shape -- letting one test drive a genuine walk -> populate
// round trip through the identical path-building logic on both sides.

#include "unity.h"
#include "bb_serialize.h"

#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------------
// mock_store_t -- shared path-keyed backing store (see file banner)
// ---------------------------------------------------------------------------

#define MOCK_MAX_ENTRIES    64
#define MOCK_MAX_CONTAINERS 32
#define MOCK_KEY_MAX        64
#define MOCK_STR_MAX        64
#define MOCK_STACK_MAX      8

typedef enum { KV_I64, KV_U64, KV_F64, KV_BOOL, KV_STR } kv_kind_t;

typedef struct {
    char      path[MOCK_KEY_MAX];
    kv_kind_t kind;
    union {
        int64_t  i64;
        uint64_t u64;
        double   f64;
        bool     b;
    } num;
    char   str[MOCK_STR_MAX];
    size_t str_len;
} kv_entry_t;

typedef struct {
    char   path[MOCK_KEY_MAX];
    bool   is_arr;
    size_t count;
} container_entry_t;

typedef struct {
    kv_entry_t        entries[MOCK_MAX_ENTRIES];
    size_t            n_entries;
    container_entry_t containers[MOCK_MAX_CONTAINERS];
    size_t            n_containers;

    // Scope stack, shared by both the recording (emit) side and the
    // reading (populate) side -- each pushes/pops identically, so a value
    // recorded by a walk() call resolves to the exact same path a later
    // populate() call will look up.
    char   stack[MOCK_STACK_MAX][MOCK_KEY_MAX];
    size_t depth;
    size_t arr_idx[MOCK_STACK_MAX];
} mock_store_t;

static void mock_reset(mock_store_t *m) { memset(m, 0, sizeof(*m)); }

// Computes the full path for `key` under the current scope. key == NULL
// means an array element (no wire key) -- consumes (and bumps) the current
// scope's array-index counter, mirroring emit's null-for-array-element
// convention on the read side too.
static void mock_next_path(mock_store_t *m, const char *key, char *out, size_t cap)
{
    const char *base = m->depth ? m->stack[m->depth - 1] : "";
    if (key) {
        if (base[0]) {
            snprintf(out, cap, "%s.%s", base, key);
        } else {
            snprintf(out, cap, "%s", key);
        }
    } else {
        size_t idx = m->depth ? m->arr_idx[m->depth - 1]++ : 0;
        snprintf(out, cap, "%s[%zu]", base, idx);
    }
}

static void mock_push(mock_store_t *m, const char *path)
{
    TEST_ASSERT_TRUE(m->depth < MOCK_STACK_MAX);
    strncpy(m->stack[m->depth], path, MOCK_KEY_MAX - 1);
    m->stack[m->depth][MOCK_KEY_MAX - 1] = '\0';
    m->arr_idx[m->depth] = 0;
    m->depth++;
}

static void mock_pop(mock_store_t *m)
{
    if (m->depth) m->depth--;
}

static kv_entry_t *mock_find(mock_store_t *m, const char *path)
{
    for (size_t i = 0; i < m->n_entries; i++) {
        if (strcmp(m->entries[i].path, path) == 0) return &m->entries[i];
    }
    return NULL;
}

static kv_entry_t *mock_upsert(mock_store_t *m, const char *path, kv_kind_t kind)
{
    kv_entry_t *e = mock_find(m, path);
    if (!e) {
        TEST_ASSERT_TRUE(m->n_entries < MOCK_MAX_ENTRIES);
        e = &m->entries[m->n_entries++];
        strncpy(e->path, path, MOCK_KEY_MAX - 1);
        e->path[MOCK_KEY_MAX - 1] = '\0';
    }
    e->kind = kind;
    return e;
}

static void mock_set_i64(mock_store_t *m, const char *path, int64_t v)
{
    mock_upsert(m, path, KV_I64)->num.i64 = v;
}
static void mock_set_u64(mock_store_t *m, const char *path, uint64_t v)
{
    mock_upsert(m, path, KV_U64)->num.u64 = v;
}
static void mock_set_f64(mock_store_t *m, const char *path, double v)
{
    mock_upsert(m, path, KV_F64)->num.f64 = v;
}
static void mock_set_bool(mock_store_t *m, const char *path, bool v)
{
    mock_upsert(m, path, KV_BOOL)->num.b = v;
}
static void mock_set_str(mock_store_t *m, const char *path, const char *s, size_t len)
{
    kv_entry_t *e = mock_upsert(m, path, KV_STR);
    size_t n = len < sizeof(e->str) - 1 ? len : sizeof(e->str) - 1;
    if (n) memcpy(e->str, s, n);
    e->str[n] = '\0';
    e->str_len = len;
}

static container_entry_t *mock_find_container(mock_store_t *m, const char *path)
{
    for (size_t i = 0; i < m->n_containers; i++) {
        if (strcmp(m->containers[i].path, path) == 0) return &m->containers[i];
    }
    return NULL;
}

static container_entry_t *mock_add_container(mock_store_t *m, const char *path, bool is_arr, size_t count)
{
    container_entry_t *c = mock_find_container(m, path);
    if (!c) {
        TEST_ASSERT_TRUE(m->n_containers < MOCK_MAX_CONTAINERS);
        c = &m->containers[m->n_containers++];
        strncpy(c->path, path, MOCK_KEY_MAX - 1);
        c->path[MOCK_KEY_MAX - 1] = '\0';
    }
    c->is_arr = is_arr;
    c->count = count;
    return c;
}

// ---------------------------------------------------------------------------
// Recording side (bb_serialize_emit_t) -- walk() a golden struct into the
// store, used by the round-trip test.
// ---------------------------------------------------------------------------

static void rec_begin_obj(void *ctx, const char *key)
{
    mock_store_t *m = ctx;
    char path[MOCK_KEY_MAX];
    mock_next_path(m, key, path, sizeof(path));
    mock_add_container(m, path, false, 0);
    mock_push(m, path);
}

static void rec_end_obj(void *ctx)
{
    mock_store_t *m = ctx;
    mock_pop(m);
}

static void rec_begin_arr(void *ctx, const char *key)
{
    mock_store_t *m = ctx;
    char path[MOCK_KEY_MAX];
    mock_next_path(m, key, path, sizeof(path));
    mock_add_container(m, path, true, 0);
    mock_push(m, path);
}

static void rec_end_arr(void *ctx)
{
    mock_store_t *m = ctx;
    size_t count = m->depth ? m->arr_idx[m->depth - 1] : 0;
    const char *path = m->depth ? m->stack[m->depth - 1] : "";
    mock_add_container(m, path, true, count);
    mock_pop(m);
}

static void rec_emit_i64(void *ctx, const char *key, int64_t v)
{
    mock_store_t *m = ctx;
    char path[MOCK_KEY_MAX];
    mock_next_path(m, key, path, sizeof(path));
    mock_set_i64(m, path, v);
}

static void rec_emit_u64(void *ctx, const char *key, uint64_t v)
{
    mock_store_t *m = ctx;
    char path[MOCK_KEY_MAX];
    mock_next_path(m, key, path, sizeof(path));
    mock_set_u64(m, path, v);
}

static void rec_emit_f64(void *ctx, const char *key, double v)
{
    mock_store_t *m = ctx;
    char path[MOCK_KEY_MAX];
    mock_next_path(m, key, path, sizeof(path));
    mock_set_f64(m, path, v);
}

static void rec_emit_bool(void *ctx, const char *key, bool v)
{
    mock_store_t *m = ctx;
    char path[MOCK_KEY_MAX];
    mock_next_path(m, key, path, sizeof(path));
    mock_set_bool(m, path, v);
}

static void rec_emit_str(void *ctx, const char *key, const char *s, size_t len)
{
    mock_store_t *m = ctx;
    char path[MOCK_KEY_MAX];
    mock_next_path(m, key, path, sizeof(path));
    mock_set_str(m, path, s, len);
}

// A NULL-valued field is genuinely absent -- no entry recorded, matching
// the presence contract populate expects on the read side.
static void rec_emit_null(void *ctx, const char *key)
{
    (void)ctx;
    (void)key;
}

static const bb_serialize_emit_t s_rec_emit = {
    .format_id = BB_FORMAT_NONE,
    .ctx = NULL,  // set per-test to &store
    .begin_obj = rec_begin_obj,
    .end_obj = rec_end_obj,
    .begin_arr = rec_begin_arr,
    .end_arr = rec_end_arr,
    .emit_i64 = rec_emit_i64,
    .emit_u64 = rec_emit_u64,
    .emit_f64 = rec_emit_f64,
    .emit_bool = rec_emit_bool,
    .emit_str = rec_emit_str,
    .emit_null = rec_emit_null,
};

// ---------------------------------------------------------------------------
// Reading side (bb_serialize_populate_t) -- pulls from the same store.
// ---------------------------------------------------------------------------

static bool src_get_i64(void *ctx, const char *key, int64_t *out)
{
    mock_store_t *m = ctx;
    char path[MOCK_KEY_MAX];
    mock_next_path(m, key, path, sizeof(path));
    kv_entry_t *e = mock_find(m, path);
    if (!e || e->kind != KV_I64) return false;
    *out = e->num.i64;
    return true;
}

static bool src_get_u64(void *ctx, const char *key, uint64_t *out)
{
    mock_store_t *m = ctx;
    char path[MOCK_KEY_MAX];
    mock_next_path(m, key, path, sizeof(path));
    kv_entry_t *e = mock_find(m, path);
    if (!e || e->kind != KV_U64) return false;
    *out = e->num.u64;
    return true;
}

static bool src_get_f64(void *ctx, const char *key, double *out)
{
    mock_store_t *m = ctx;
    char path[MOCK_KEY_MAX];
    mock_next_path(m, key, path, sizeof(path));
    kv_entry_t *e = mock_find(m, path);
    if (!e || e->kind != KV_F64) return false;
    *out = e->num.f64;
    return true;
}

static bool src_get_bool(void *ctx, const char *key, bool *out)
{
    mock_store_t *m = ctx;
    char path[MOCK_KEY_MAX];
    mock_next_path(m, key, path, sizeof(path));
    kv_entry_t *e = mock_find(m, path);
    if (!e || e->kind != KV_BOOL) return false;
    *out = e->num.b;
    return true;
}

static bool src_get_str(void *ctx, const char *key, char *dst, size_t cap, size_t *out_len)
{
    mock_store_t *m = ctx;
    char path[MOCK_KEY_MAX];
    mock_next_path(m, key, path, sizeof(path));
    kv_entry_t *e = mock_find(m, path);
    if (!e || e->kind != KV_STR) return false;

    // Bounded write into `dst`, capacity `cap` (the field's max_len) -- the
    // getter's own responsibility per the populate contract.
    size_t n = e->str_len < cap ? e->str_len : cap;
    if (n && cap) memcpy(dst, e->str, n);
    if (out_len) *out_len = n;
    return true;
}

static bool src_begin_obj(void *ctx, const char *key)
{
    mock_store_t *m = ctx;
    char path[MOCK_KEY_MAX];
    mock_next_path(m, key, path, sizeof(path));
    container_entry_t *c = mock_find_container(m, path);
    if (!c || c->is_arr) return false;
    mock_push(m, path);
    return true;
}

static bool src_end_obj(void *ctx)
{
    mock_pop((mock_store_t *)ctx);
    return true;
}

static bool src_begin_arr(void *ctx, const char *key, size_t *count)
{
    mock_store_t *m = ctx;
    char path[MOCK_KEY_MAX];
    mock_next_path(m, key, path, sizeof(path));
    container_entry_t *c = mock_find_container(m, path);
    if (!c || !c->is_arr) return false;
    if (count) *count = c->count;
    mock_push(m, path);
    return true;
}

static bool src_end_arr(void *ctx)
{
    mock_pop((mock_store_t *)ctx);
    return true;
}

static const bb_serialize_populate_t s_src = {
    .format_id = BB_FORMAT_NONE,
    .ctx = NULL,  // set per-test to &store
    .get_i64 = src_get_i64,
    .get_u64 = src_get_u64,
    .get_f64 = src_get_f64,
    .get_bool = src_get_bool,
    .get_str = src_get_str,
    .begin_obj = src_begin_obj,
    .end_obj = src_end_obj,
    .begin_arr = src_begin_arr,
    .end_arr = src_end_arr,
};

static bb_serialize_populate_t make_src(mock_store_t *m)
{
    bb_serialize_populate_t s = s_src;
    s.ctx = m;
    return s;
}

static bb_serialize_emit_t make_emit(mock_store_t *m)
{
    bb_serialize_emit_t e = s_rec_emit;
    e.ctx = m;
    return e;
}

// ---------------------------------------------------------------------------
// 1. flat scalars -- each type's happy path
// ---------------------------------------------------------------------------

typedef struct {
    int64_t  i;
    uint64_t u;
    double   f;
    bool     b;
    char     s[16];
} flat_snap_t;

static const bb_serialize_field_t s_flat_fields[] = {
    { .key = "i", .type = BB_TYPE_I64, .offset = offsetof(flat_snap_t, i) },
    { .key = "u", .type = BB_TYPE_U64, .offset = offsetof(flat_snap_t, u) },
    { .key = "f", .type = BB_TYPE_F64, .offset = offsetof(flat_snap_t, f) },
    { .key = "b", .type = BB_TYPE_BOOL, .offset = offsetof(flat_snap_t, b) },
    { .key = "s", .type = BB_TYPE_STR, .offset = offsetof(flat_snap_t, s), .max_len = 16 },
};

static const bb_serialize_desc_t s_flat_desc = {
    .type_name = "flat_snap_t",
    .fields = s_flat_fields,
    .n_fields = 5,
    .snap_size = sizeof(flat_snap_t),
};

void test_bb_serialize_populate_flat_scalars(void)
{
    mock_store_t m;
    mock_reset(&m);
    mock_set_i64(&m, "i", -7);
    mock_set_u64(&m, "u", 42);
    mock_set_f64(&m, "f", 3.5);
    mock_set_bool(&m, "b", true);
    mock_set_str(&m, "s", "hello", 5);

    bb_serialize_populate_t src = make_src(&m);
    flat_snap_t dst = { 0 };

    bb_err_t rc = bb_serialize_populate(&s_flat_desc, &dst, &src);

    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL_INT64(-7, dst.i);
    TEST_ASSERT_EQUAL_UINT64(42, dst.u);
    TEST_ASSERT_EQUAL_DOUBLE(3.5, dst.f);
    TEST_ASSERT_TRUE(dst.b);
    TEST_ASSERT_EQUAL_STRING("hello", dst.s);
}

// ---------------------------------------------------------------------------
// 2. absent field keeps the zero-initialized default -- no entry in the
// store means the getter returns false, so populate never writes.
// ---------------------------------------------------------------------------

void test_bb_serialize_populate_absent_field_keeps_default(void)
{
    mock_store_t m;
    mock_reset(&m);
    // Every field deliberately absent -- including "i", so get_i64's own
    // false branch (never exercised by the happy-path test) is covered too.

    bb_serialize_populate_t src = make_src(&m);
    flat_snap_t dst = { .i = 55, .u = 111, .f = 2.0, .b = true, .s = "keepme" };

    bb_err_t rc = bb_serialize_populate(&s_flat_desc, &dst, &src);

    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL_INT64(55, dst.i);
    TEST_ASSERT_EQUAL_UINT64(111, dst.u);
    TEST_ASSERT_EQUAL_DOUBLE(2.0, dst.f);
    TEST_ASSERT_TRUE(dst.b);
    TEST_ASSERT_EQUAL_STRING("keepme", dst.s);
}

// ---------------------------------------------------------------------------
// 3. STR truncation at max_len -- a stored value longer than the field's
// buffer is bounded, never overflows the destination.
// ---------------------------------------------------------------------------

typedef struct {
    char status[4];
} str_snap_t;

static const bb_serialize_field_t s_str_fields[] = {
    { .key = "status", .type = BB_TYPE_STR, .offset = offsetof(str_snap_t, status), .max_len = 4 },
};

static const bb_serialize_desc_t s_str_desc = {
    .type_name = "str_snap_t",
    .fields = s_str_fields,
    .n_fields = 1,
    .snap_size = sizeof(str_snap_t),
};

void test_bb_serialize_populate_str_truncation_at_max_len(void)
{
    mock_store_t m;
    mock_reset(&m);
    mock_set_str(&m, "status", "abcdefgh", 8);  // longer than the 4-byte field

    bb_serialize_populate_t src = make_src(&m);
    str_snap_t dst;
    memset(&dst, 0xAA, sizeof(dst));

    bb_err_t rc = bb_serialize_populate(&s_str_desc, &dst, &src);

    TEST_ASSERT_EQUAL(BB_OK, rc);
    // Bounded to exactly max_len bytes -- never an OOB write past status[4].
    TEST_ASSERT_EQUAL_UINT8('a', (uint8_t)dst.status[0]);
    TEST_ASSERT_EQUAL_UINT8('d', (uint8_t)dst.status[3]);
}

// ---------------------------------------------------------------------------
// 4. nested OBJ -- present container, child offsets resolved relative to it.
// ---------------------------------------------------------------------------

typedef struct {
    int64_t x;
    int64_t y;
} point_t;

typedef struct {
    int64_t id;
    point_t pos;
} nested_snap_t;

static const bb_serialize_field_t s_point_fields[] = {
    { .key = "x", .type = BB_TYPE_I64, .offset = offsetof(point_t, x) },
    { .key = "y", .type = BB_TYPE_I64, .offset = offsetof(point_t, y) },
};

static const bb_serialize_field_t s_nested_fields[] = {
    { .key = "id", .type = BB_TYPE_I64, .offset = offsetof(nested_snap_t, id) },
    { .key = "pos", .type = BB_TYPE_OBJ, .offset = offsetof(nested_snap_t, pos),
      .children = s_point_fields, .n_children = 2 },
};

static const bb_serialize_desc_t s_nested_desc = {
    .type_name = "nested_snap_t",
    .fields = s_nested_fields,
    .n_fields = 2,
    .snap_size = sizeof(nested_snap_t),
};

void test_bb_serialize_populate_nested_obj(void)
{
    mock_store_t m;
    mock_reset(&m);
    mock_set_i64(&m, "id", 1);
    mock_add_container(&m, "pos", false, 0);
    mock_set_i64(&m, "pos.x", 10);
    mock_set_i64(&m, "pos.y", 20);

    bb_serialize_populate_t src = make_src(&m);
    nested_snap_t dst = { 0 };

    bb_err_t rc = bb_serialize_populate(&s_nested_desc, &dst, &src);

    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL_INT64(1, dst.id);
    TEST_ASSERT_EQUAL_INT64(10, dst.pos.x);
    TEST_ASSERT_EQUAL_INT64(20, dst.pos.y);
}

// ---------------------------------------------------------------------------
// 5. absent nested OBJ -- container missing entirely -> child bytes stay
// untouched, no crash, no orphan push/pop.
// ---------------------------------------------------------------------------

void test_bb_serialize_populate_nested_obj_absent_leaves_untouched(void)
{
    mock_store_t m;
    mock_reset(&m);
    mock_set_i64(&m, "id", 1);
    // "pos" container deliberately absent.

    bb_serialize_populate_t src = make_src(&m);
    nested_snap_t dst = { .id = 0, .pos = { .x = -1, .y = -1 } };

    bb_err_t rc = bb_serialize_populate(&s_nested_desc, &dst, &src);

    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL_INT64(1, dst.id);
    TEST_ASSERT_EQUAL_INT64(-1, dst.pos.x);
    TEST_ASSERT_EQUAL_INT64(-1, dst.pos.y);
}

// ---------------------------------------------------------------------------
// 6. ARR of OBJ -- caller-prewired element storage, capacity == max_items.
// ---------------------------------------------------------------------------

typedef struct {
    int64_t id;
    int64_t val;
} elem_t;

typedef struct {
    bb_serialize_arr_t items;
} arr_obj_snap_t;

static const bb_serialize_field_t s_elem_fields[] = {
    { .key = "id", .type = BB_TYPE_I64, .offset = offsetof(elem_t, id) },
    { .key = "val", .type = BB_TYPE_I64, .offset = offsetof(elem_t, val) },
};

static const bb_serialize_field_t s_arr_obj_fields[] = {
    { .key = "items", .type = BB_TYPE_ARR, .offset = offsetof(arr_obj_snap_t, items),
      .elem_type = BB_TYPE_OBJ, .elem_size = sizeof(elem_t), .max_items = 4,
      .children = s_elem_fields, .n_children = 2 },
};

static const bb_serialize_desc_t s_arr_obj_desc = {
    .type_name = "arr_obj_snap_t",
    .fields = s_arr_obj_fields,
    .n_fields = 1,
    .snap_size = sizeof(arr_obj_snap_t),
};

void test_bb_serialize_populate_arr_of_obj(void)
{
    mock_store_t m;
    mock_reset(&m);
    mock_add_container(&m, "items", true, 2);
    mock_add_container(&m, "items[0]", false, 0);
    mock_set_i64(&m, "items[0].id", 1);
    mock_set_i64(&m, "items[0].val", 100);
    mock_add_container(&m, "items[1]", false, 0);
    mock_set_i64(&m, "items[1].id", 2);
    mock_set_i64(&m, "items[1].val", 200);

    bb_serialize_populate_t src = make_src(&m);
    elem_t storage[4] = { 0 };
    arr_obj_snap_t dst = { .items = { .items = storage, .count = 0 } };

    bb_err_t rc = bb_serialize_populate(&s_arr_obj_desc, &dst, &src);

    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL_UINT(2, dst.items.count);
    TEST_ASSERT_EQUAL_INT64(1, storage[0].id);
    TEST_ASSERT_EQUAL_INT64(100, storage[0].val);
    TEST_ASSERT_EQUAL_INT64(2, storage[1].id);
    TEST_ASSERT_EQUAL_INT64(200, storage[1].val);
}

// ---------------------------------------------------------------------------
// 7. ARR absent -> the whole bb_serialize_arr_t carrier (items AND count)
// stays byte-for-byte untouched.
// ---------------------------------------------------------------------------

void test_bb_serialize_populate_arr_absent_leaves_untouched(void)
{
    mock_store_t m;
    mock_reset(&m);
    // "items" container deliberately absent.

    bb_serialize_populate_t src = make_src(&m);
    elem_t storage[4] = { 0 };
    arr_obj_snap_t dst = { .items = { .items = storage, .count = 42 } };

    bb_err_t rc = bb_serialize_populate(&s_arr_obj_desc, &dst, &src);

    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL_UINT(42, dst.items.count);
    TEST_ASSERT_EQUAL_PTR(storage, dst.items.items);
}

// ---------------------------------------------------------------------------
// 7b. ARR present but `.items` is NULL (nothing pre-wired to write into) --
// degrades to zero elements rather than dereferencing, distinct from the
// container-absent case above (this exercises begin_arr's TRUE path with a
// NULL carrier).
// ---------------------------------------------------------------------------

void test_bb_serialize_populate_arr_present_null_items_degrades_to_zero(void)
{
    mock_store_t m;
    mock_reset(&m);
    mock_add_container(&m, "items", true, 2);
    mock_add_container(&m, "items[0]", false, 0);
    mock_set_i64(&m, "items[0].id", 1);
    mock_set_i64(&m, "items[0].val", 100);

    bb_serialize_populate_t src = make_src(&m);
    arr_obj_snap_t dst = { .items = { .items = NULL, .count = 0 } };

    bb_err_t rc = bb_serialize_populate(&s_arr_obj_desc, &dst, &src);

    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL_UINT(0, dst.items.count);
    TEST_ASSERT_NULL(dst.items.items);
}

// ---------------------------------------------------------------------------
// 7c. ARR of OBJ: a source begin_obj() miss mid-array stops the loop early,
// same convention as the ARR-of-STR mid-array miss above but for the
// distinct BB_TYPE_OBJ element code path.
// ---------------------------------------------------------------------------

void test_bb_serialize_populate_arr_of_obj_stops_on_first_miss(void)
{
    mock_store_t m;
    mock_reset(&m);
    mock_add_container(&m, "items", true, 2);
    mock_add_container(&m, "items[0]", false, 0);
    mock_set_i64(&m, "items[0].id", 1);
    mock_set_i64(&m, "items[0].val", 100);
    // "items[1]" container deliberately absent -- loop must stop here.

    bb_serialize_populate_t src = make_src(&m);
    elem_t storage[4] = { 0 };
    arr_obj_snap_t dst = { .items = { .items = storage, .count = 0 } };

    bb_err_t rc = bb_serialize_populate(&s_arr_obj_desc, &dst, &src);

    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL_UINT(1, dst.items.count);
    TEST_ASSERT_EQUAL_INT64(1, storage[0].id);
}

// ---------------------------------------------------------------------------
// 8. ARR capacity bound -- source reports more elements than max_items;
// populate stops at the pre-wired capacity, never overflows storage.
// ---------------------------------------------------------------------------

typedef struct {
    bb_serialize_arr_t items;
} arr_obj_small_cap_snap_t;

static const bb_serialize_field_t s_arr_obj_small_cap_fields[] = {
    { .key = "items", .type = BB_TYPE_ARR, .offset = offsetof(arr_obj_small_cap_snap_t, items),
      .elem_type = BB_TYPE_OBJ, .elem_size = sizeof(elem_t), .max_items = 2,
      .children = s_elem_fields, .n_children = 2 },
};

static const bb_serialize_desc_t s_arr_obj_small_cap_desc = {
    .type_name = "arr_obj_small_cap_snap_t",
    .fields = s_arr_obj_small_cap_fields,
    .n_fields = 1,
    .snap_size = sizeof(arr_obj_small_cap_snap_t),
};

void test_bb_serialize_populate_arr_capacity_bounds_at_max_items(void)
{
    mock_store_t m;
    mock_reset(&m);
    mock_add_container(&m, "items", true, 5);  // source claims 5 elements
    for (int i = 0; i < 5; i++) {
        char path[MOCK_KEY_MAX];
        snprintf(path, sizeof(path), "items[%d]", i);
        mock_add_container(&m, path, false, 0);
        snprintf(path, sizeof(path), "items[%d].id", i);
        mock_set_i64(&m, path, i);
        snprintf(path, sizeof(path), "items[%d].val", i);
        mock_set_i64(&m, path, i * 10);
    }

    bb_serialize_populate_t src = make_src(&m);
    elem_t storage[2] = { 0 };  // capacity 2, matching max_items
    arr_obj_small_cap_snap_t dst = { .items = { .items = storage, .count = 0 } };

    bb_err_t rc = bb_serialize_populate(&s_arr_obj_small_cap_desc, &dst, &src);

    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL_UINT(2, dst.items.count);  // capped, not 5
    TEST_ASSERT_EQUAL_INT64(0, storage[0].id);
    TEST_ASSERT_EQUAL_INT64(1, storage[1].id);
}

// ---------------------------------------------------------------------------
// 8b. ARR max_items == 0 is a misconfiguration for populate (diverges from
// JSON's 0-means-unbounded) -- rejected LOUD as BB_ERR_INVALID_ARG by the
// pre-flight scan, before any scatter, even though the array is present,
// non-NULL, and the source reports a nonzero count.
// ---------------------------------------------------------------------------

typedef struct {
    bb_serialize_arr_t items;
} arr_obj_zero_cap_snap_t;

static const bb_serialize_field_t s_arr_obj_zero_cap_fields[] = {
    { .key = "items", .type = BB_TYPE_ARR, .offset = offsetof(arr_obj_zero_cap_snap_t, items),
      .elem_type = BB_TYPE_OBJ, .elem_size = sizeof(elem_t), .max_items = 0,
      .children = s_elem_fields, .n_children = 2 },
};

static const bb_serialize_desc_t s_arr_obj_zero_cap_desc = {
    .type_name = "arr_obj_zero_cap_snap_t",
    .fields = s_arr_obj_zero_cap_fields,
    .n_fields = 1,
    .snap_size = sizeof(arr_obj_zero_cap_snap_t),
};

void test_bb_serialize_populate_arr_max_items_zero_returns_invalid_arg(void)
{
    mock_store_t m;
    mock_reset(&m);
    mock_add_container(&m, "items", true, 3);  // present, source reports 3 elements
    mock_add_container(&m, "items[0]", false, 0);
    mock_set_i64(&m, "items[0].id", 1);
    mock_set_i64(&m, "items[0].val", 100);

    bb_serialize_populate_t src = make_src(&m);
    elem_t storage[4] = { 0 };
    arr_obj_zero_cap_snap_t dst = { .items = { .items = storage, .count = 42 } };

    bb_err_t rc = bb_serialize_populate(&s_arr_obj_zero_cap_desc, &dst, &src);

    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, rc);
    // Rejected during the pre-flight scan, before any scatter -- dst is
    // fully untouched.
    TEST_ASSERT_EQUAL_UINT(42, dst.items.count);
    TEST_ASSERT_EQUAL_PTR(storage, dst.items.items);
}

// ---------------------------------------------------------------------------
// 9. ARR of STR -- caller-prewired array of writable char* buffers.
// ---------------------------------------------------------------------------

typedef struct {
    bb_serialize_arr_t tags;
} arr_str_snap_t;

static const bb_serialize_field_t s_arr_str_fields[] = {
    { .key = "tags", .type = BB_TYPE_ARR, .offset = offsetof(arr_str_snap_t, tags),
      .elem_type = BB_TYPE_STR, .max_len = 8, .max_items = 3 },
};

static const bb_serialize_desc_t s_arr_str_desc = {
    .type_name = "arr_str_snap_t",
    .fields = s_arr_str_fields,
    .n_fields = 1,
    .snap_size = sizeof(arr_str_snap_t),
};

void test_bb_serialize_populate_arr_of_str(void)
{
    mock_store_t m;
    mock_reset(&m);
    mock_add_container(&m, "tags", true, 2);
    mock_set_str(&m, "tags[0]", "one", 3);
    mock_set_str(&m, "tags[1]", "two", 3);

    bb_serialize_populate_t src = make_src(&m);
    char        buf0[8] = { 0 };
    char        buf1[8] = { 0 };
    char        buf2[8] = { 0 };
    char       *ptrs[3] = { buf0, buf1, buf2 };
    arr_str_snap_t dst = { .tags = { .items = ptrs, .count = 0 } };

    bb_err_t rc = bb_serialize_populate(&s_arr_str_desc, &dst, &src);

    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL_UINT(2, dst.tags.count);
    TEST_ASSERT_EQUAL_UINT8('o', (uint8_t)buf0[0]);
    TEST_ASSERT_EQUAL_UINT8('t', (uint8_t)buf1[0]);
}

// ---------------------------------------------------------------------------
// 10. ARR of STR: a source getter miss mid-array stops the loop early --
// the returned count reflects only what was actually written, no gap
// followed by more writes.
// ---------------------------------------------------------------------------

void test_bb_serialize_populate_arr_of_str_stops_on_first_miss(void)
{
    mock_store_t m;
    mock_reset(&m);
    mock_add_container(&m, "tags", true, 3);
    mock_set_str(&m, "tags[0]", "one", 3);
    // tags[1] deliberately absent -- loop must stop here.
    mock_set_str(&m, "tags[2]", "three", 5);

    bb_serialize_populate_t src = make_src(&m);
    char        buf0[8] = { 0 };
    char        buf1[8] = { 0 };
    char        buf2[8] = { 0 };
    char       *ptrs[3] = { buf0, buf1, buf2 };
    arr_str_snap_t dst = { .tags = { .items = ptrs, .count = 0 } };

    bb_err_t rc = bb_serialize_populate(&s_arr_str_desc, &dst, &src);

    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL_UINT(1, dst.tags.count);
}

// ---------------------------------------------------------------------------
// 11. depth guard (OBJ): self-referential descriptor bails at
// BB_SERIALIZE_MAX_DEPTH -- returns BB_ERR_NO_SPACE, never overflows the
// stack, never proceeds as if nothing happened.
// ---------------------------------------------------------------------------

static bool probe_get_i64(void *ctx, const char *key, int64_t *out)
{
    (void)ctx;
    (void)key;
    *out = 1;
    return true;
}
static bool probe_get_u64(void *ctx, const char *key, uint64_t *out)
{
    (void)ctx;
    (void)key;
    (void)out;
    return false;
}
static bool probe_get_f64(void *ctx, const char *key, double *out)
{
    (void)ctx;
    (void)key;
    (void)out;
    return false;
}
static bool probe_get_bool(void *ctx, const char *key, bool *out)
{
    (void)ctx;
    (void)key;
    (void)out;
    return false;
}
static bool probe_get_str(void *ctx, const char *key, char *dst, size_t cap, size_t *out_len)
{
    (void)ctx;
    (void)key;
    (void)dst;
    (void)cap;
    (void)out_len;
    return false;
}
static bool probe_begin_obj(void *ctx, const char *key)
{
    (void)ctx;
    (void)key;
    return true;  // always present -- forces unbounded descent
}
static bool probe_end_obj(void *ctx)
{
    (void)ctx;
    return true;
}
static bool probe_begin_arr(void *ctx, const char *key, size_t *count)
{
    (void)ctx;
    (void)key;
    if (count) *count = 1;
    return true;  // always present, one element -- forces unbounded descent
}
static bool probe_end_arr(void *ctx)
{
    (void)ctx;
    return true;
}

static const bb_serialize_populate_t s_probe_src = {
    .format_id = BB_FORMAT_NONE,
    .ctx = NULL,
    .get_i64 = probe_get_i64,
    .get_u64 = probe_get_u64,
    .get_f64 = probe_get_f64,
    .get_bool = probe_get_bool,
    .get_str = probe_get_str,
    .begin_obj = probe_begin_obj,
    .end_obj = probe_end_obj,
    .begin_arr = probe_begin_arr,
    .end_arr = probe_end_arr,
};

// ---------------------------------------------------------------------------
// 10b. STR get_str false return leaves dst untouched -- uses the
// always-false probe getter directly (distinct from test 2's
// mock_find-natural-miss route), pinning the getter contract: a get_str
// returning false must leave dst at its pre-populate default (the probe
// getter never writes on false regardless of whether the return is checked).
// ---------------------------------------------------------------------------

void test_bb_serialize_populate_str_get_false_leaves_default(void)
{
    flat_snap_t dst = { .i = 0, .u = 111, .f = 2.0, .b = true, .s = "keepme" };

    // s_probe_src: get_i64 always returns true (writes 1); get_u64/get_f64/
    // get_bool/get_str always return false -- so only "i" changes.
    bb_err_t rc = bb_serialize_populate(&s_flat_desc, &dst, &s_probe_src);

    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL_INT64(1, dst.i);
    TEST_ASSERT_EQUAL_UINT64(111, dst.u);
    TEST_ASSERT_EQUAL_DOUBLE(2.0, dst.f);
    TEST_ASSERT_TRUE(dst.b);
    TEST_ASSERT_EQUAL_STRING("keepme", dst.s);
}

typedef struct {
    int64_t marker;
} deep_snap_t;

static const bb_serialize_field_t s_deep_fields[] = {
    { .key = "marker", .type = BB_TYPE_I64, .offset = offsetof(deep_snap_t, marker) },
    { .key = "deep", .type = BB_TYPE_OBJ, .offset = 0,
      .children = s_deep_fields, .n_children = 2 },
};

static const bb_serialize_desc_t s_deep_desc = {
    .type_name = "deep_snap_t",
    .fields = s_deep_fields,
    .n_fields = 2,
    .snap_size = sizeof(deep_snap_t),
};

void test_bb_serialize_populate_depth_guard_returns_no_space(void)
{
    deep_snap_t dst = { 0 };

    bb_err_t rc = bb_serialize_populate(&s_deep_desc, &dst, &s_probe_src);

    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, rc);
}

// ---------------------------------------------------------------------------
// 12. depth guard (ARR-of-OBJ): distinct code path from the plain-OBJ
// guard -- a self-referential array element type recurses forever unless
// bounded. Reuses index-0 of a single-element backing buffer every level
// (begin_arr always reports count == 1), so the guard -- not storage size
// -- is what has to stop the descent.
// ---------------------------------------------------------------------------

typedef struct {
    int64_t            marker;
    bb_serialize_arr_t kids;
} deep_arr_elem_t;

static const bb_serialize_field_t s_deep_arr_fields[] = {
    { .key = "marker", .type = BB_TYPE_I64, .offset = offsetof(deep_arr_elem_t, marker) },
    { .key = "kids", .type = BB_TYPE_ARR, .offset = offsetof(deep_arr_elem_t, kids),
      .elem_type = BB_TYPE_OBJ, .elem_size = sizeof(deep_arr_elem_t), .max_items = 1,
      .children = s_deep_arr_fields, .n_children = 2 },
};

static const bb_serialize_desc_t s_deep_arr_desc = {
    .type_name = "deep_arr_elem_t",
    .fields = s_deep_arr_fields,
    .n_fields = 2,
    .snap_size = sizeof(deep_arr_elem_t),
};

void test_bb_serialize_populate_arr_of_obj_depth_guard_returns_no_space(void)
{
    deep_arr_elem_t single = { 0 };
    single.kids.items = &single;  // self-referential: every level writes the same struct
    single.kids.count = 0;

    bb_err_t rc = bb_serialize_populate(&s_deep_arr_desc, &single, &s_probe_src);

    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, rc);
}

// ---------------------------------------------------------------------------
// 13. BB_TYPE_STR_N and BB_TYPE_REF are not supported by populate -- the
// pre-flight descriptor scan rejects them LOUD (BB_ERR_UNSUPPORTED) before
// any scatter begins, leaving `dst` fully untouched, rather than the old
// silent-no-op convention.
// ---------------------------------------------------------------------------

typedef struct {
    bb_serialize_str_n_t sn;
} strn_ref_snap_t;

static const bb_serialize_field_t s_strn_ref_fields[] = {
    { .key = "sn", .type = BB_TYPE_STR_N, .offset = offsetof(strn_ref_snap_t, sn) },
    { .key = "ref", .type = BB_TYPE_REF, .ref_key = "some.ref" },
};

static const bb_serialize_desc_t s_strn_ref_desc = {
    .type_name = "strn_ref_snap_t",
    .fields = s_strn_ref_fields,
    .n_fields = 2,
    .snap_size = sizeof(strn_ref_snap_t),
};

void test_bb_serialize_populate_str_n_and_ref_are_unsupported(void)
{
    strn_ref_snap_t dst = { .sn = { .ptr = (const char *)0x1234, .len = 99 } };

    bb_err_t rc = bb_serialize_populate(&s_strn_ref_desc, &dst, &s_probe_src);

    TEST_ASSERT_EQUAL(BB_ERR_UNSUPPORTED, rc);
    // Rejected during the pre-flight scan, before any scatter -- dst is
    // fully untouched.
    TEST_ASSERT_EQUAL_PTR((const char *)0x1234, dst.sn.ptr);
    TEST_ASSERT_EQUAL_UINT(99, dst.sn.len);
}

// ---------------------------------------------------------------------------
// 13b. BB_TYPE_REF alone (no STR_N sibling) is also pre-flight-rejected --
// exercises the REF arm of the switch in populate_check_fields() directly,
// distinct from the STR_N arm covered above.
// ---------------------------------------------------------------------------

typedef struct {
    int64_t marker;
} ref_only_snap_t;

static const bb_serialize_field_t s_ref_only_fields[] = {
    { .key = "ref", .type = BB_TYPE_REF, .offset = offsetof(ref_only_snap_t, marker),
      .ref_key = "some.ref" },
};

static const bb_serialize_desc_t s_ref_only_desc = {
    .type_name = "ref_only_snap_t",
    .fields = s_ref_only_fields,
    .n_fields = 1,
    .snap_size = sizeof(ref_only_snap_t),
};

void test_bb_serialize_populate_ref_alone_is_unsupported(void)
{
    ref_only_snap_t dst = { .marker = 7 };

    bb_err_t rc = bb_serialize_populate(&s_ref_only_desc, &dst, &s_probe_src);

    TEST_ASSERT_EQUAL(BB_ERR_UNSUPPORTED, rc);
    TEST_ASSERT_EQUAL_INT64(7, dst.marker);
}

// ---------------------------------------------------------------------------
// 13c. Pre-flight rejection propagates up through a nested OBJ -- the
// violation is one level deep (inside a BB_TYPE_OBJ child), exercising the
// recursive-OBJ error-propagation branch in populate_check_fields()
// distinctly from the top-level rejections above.
// ---------------------------------------------------------------------------

typedef struct {
    bb_serialize_str_n_t sn;
} strn_only_t;

typedef struct {
    int64_t     id;
    strn_only_t child;
} nested_strn_snap_t;

static const bb_serialize_field_t s_strn_only_fields[] = {
    { .key = "sn", .type = BB_TYPE_STR_N, .offset = offsetof(strn_only_t, sn) },
};

static const bb_serialize_field_t s_nested_strn_fields[] = {
    { .key = "id", .type = BB_TYPE_I64, .offset = offsetof(nested_strn_snap_t, id) },
    { .key = "child", .type = BB_TYPE_OBJ, .offset = offsetof(nested_strn_snap_t, child),
      .children = s_strn_only_fields, .n_children = 1 },
};

static const bb_serialize_desc_t s_nested_strn_desc = {
    .type_name = "nested_strn_snap_t",
    .fields = s_nested_strn_fields,
    .n_fields = 2,
    .snap_size = sizeof(nested_strn_snap_t),
};

void test_bb_serialize_populate_str_n_in_nested_obj_is_unsupported(void)
{
    nested_strn_snap_t dst = { .id = 5 };

    bb_err_t rc = bb_serialize_populate(&s_nested_strn_desc, &dst, &s_probe_src);

    TEST_ASSERT_EQUAL(BB_ERR_UNSUPPORTED, rc);
    TEST_ASSERT_EQUAL_INT64(5, dst.id);
}

// ---------------------------------------------------------------------------
// 13d. Pre-flight rejection propagates up through an ARR-of-OBJ's element
// fields -- exercises the recursive-ARR error-propagation branch in
// populate_check_fields(), distinct from the plain-OBJ propagation above.
// ---------------------------------------------------------------------------

typedef struct {
    bb_serialize_arr_t items;
} arr_strn_snap_t;

static const bb_serialize_field_t s_arr_strn_fields[] = {
    { .key = "items", .type = BB_TYPE_ARR, .offset = offsetof(arr_strn_snap_t, items),
      .elem_type = BB_TYPE_OBJ, .elem_size = sizeof(strn_only_t), .max_items = 4,
      .children = s_strn_only_fields, .n_children = 1 },
};

static const bb_serialize_desc_t s_arr_strn_desc = {
    .type_name = "arr_strn_snap_t",
    .fields = s_arr_strn_fields,
    .n_fields = 1,
    .snap_size = sizeof(arr_strn_snap_t),
};

void test_bb_serialize_populate_str_n_in_arr_of_obj_is_unsupported(void)
{
    arr_strn_snap_t dst = { .items = { .items = NULL, .count = 0 } };

    bb_err_t rc = bb_serialize_populate(&s_arr_strn_desc, &dst, &s_probe_src);

    TEST_ASSERT_EQUAL(BB_ERR_UNSUPPORTED, rc);
}

// ---------------------------------------------------------------------------
// 14. exhaustive-enum default branch is a defensive no-op, not a crash.
// ---------------------------------------------------------------------------

void test_bb_serialize_populate_unknown_type_is_noop(void)
{
    flat_snap_t dst = { 0 };

    static const bb_serialize_field_t s_unknown_type_fields[] = {
        { .key = "bad", .type = (bb_type_t)99, .offset = 0 },
    };
    static const bb_serialize_desc_t s_unknown_type_desc = {
        .type_name = "flat_snap_t",
        .fields = s_unknown_type_fields,
        .n_fields = 1,
        .snap_size = sizeof(flat_snap_t),
    };

    bb_err_t rc = bb_serialize_populate(&s_unknown_type_desc, &dst, &s_probe_src);

    TEST_ASSERT_EQUAL(BB_OK, rc);
}

// ---------------------------------------------------------------------------
// 15. NULL args -> BB_ERR_INVALID_ARG, never a crash.
// ---------------------------------------------------------------------------

void test_bb_serialize_populate_null_args_returns_invalid_arg(void)
{
    flat_snap_t dst = { 0 };

    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_serialize_populate(NULL, &dst, &s_probe_src));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_serialize_populate(&s_flat_desc, NULL, &s_probe_src));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_serialize_populate(&s_flat_desc, &dst, NULL));
}

// ---------------------------------------------------------------------------
// 16. walk -> populate round trip: a golden struct is walked into the
// store, then a fresh zero struct is populated back out of it -- proving
// the same descriptor drives both directions to an identical result.
// ---------------------------------------------------------------------------

typedef struct {
    int64_t             id;
    point_t             pos;
    bb_serialize_arr_t  items;
} roundtrip_snap_t;

static const bb_serialize_field_t s_roundtrip_fields[] = {
    { .key = "id", .type = BB_TYPE_I64, .offset = offsetof(roundtrip_snap_t, id) },
    { .key = "pos", .type = BB_TYPE_OBJ, .offset = offsetof(roundtrip_snap_t, pos),
      .children = s_point_fields, .n_children = 2 },
    { .key = "items", .type = BB_TYPE_ARR, .offset = offsetof(roundtrip_snap_t, items),
      .elem_type = BB_TYPE_OBJ, .elem_size = sizeof(elem_t), .max_items = 4,
      .children = s_elem_fields, .n_children = 2 },
};

static const bb_serialize_desc_t s_roundtrip_desc = {
    .type_name = "roundtrip_snap_t",
    .fields = s_roundtrip_fields,
    .n_fields = 3,
    .snap_size = sizeof(roundtrip_snap_t),
};

void test_bb_serialize_populate_walk_roundtrip(void)
{
    mock_store_t m;
    mock_reset(&m);

    elem_t golden_elems[2] = { { .id = 1, .val = 100 }, { .id = 2, .val = 200 } };
    roundtrip_snap_t golden = {
        .id = 42,
        .pos = { .x = 7, .y = -3 },
        .items = { .items = golden_elems, .count = 2 },
    };

    bb_serialize_emit_t emit = make_emit(&m);
    bb_serialize_walk(&s_roundtrip_desc, &golden, &emit);

    bb_serialize_populate_t src = make_src(&m);
    elem_t            storage[4] = { 0 };
    roundtrip_snap_t  fresh = { .items = { .items = storage, .count = 0 } };

    bb_err_t rc = bb_serialize_populate(&s_roundtrip_desc, &fresh, &src);

    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL_INT64(golden.id, fresh.id);
    TEST_ASSERT_EQUAL_INT64(golden.pos.x, fresh.pos.x);
    TEST_ASSERT_EQUAL_INT64(golden.pos.y, fresh.pos.y);
    TEST_ASSERT_EQUAL_UINT(golden.items.count, fresh.items.count);
    TEST_ASSERT_EQUAL_INT64(golden_elems[0].id, storage[0].id);
    TEST_ASSERT_EQUAL_INT64(golden_elems[0].val, storage[0].val);
    TEST_ASSERT_EQUAL_INT64(golden_elems[1].id, storage[1].id);
    TEST_ASSERT_EQUAL_INT64(golden_elems[1].val, storage[1].val);
}
