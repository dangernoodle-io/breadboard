#include "unity.h"
#include "bb_init.h"
#include "bb_mem_test.h"
#include <stdbool.h>
#include <string.h>

// Test counters and flags
static int s_init_call_count = 0;
static bool s_init_flags[10];

// PRE_HTTP test counters
static int s_pre_http_call_count = 0;
static bool s_pre_http_flags[10];
static int s_pre_http_order[10];
static int s_pre_http_order_idx = 0;

// Fake init functions
static bb_err_t fake_init_1(bb_http_handle_t server)
{
    (void)server;
    s_init_flags[0] = true;
    s_init_call_count++;
    return BB_OK;
}

static bb_err_t fake_init_2(bb_http_handle_t server)
{
    (void)server;
    s_init_flags[1] = true;
    s_init_call_count++;
    return BB_OK;
}

static bb_err_t fake_init_3_error(bb_http_handle_t server)
{
    (void)server;
    s_init_flags[2] = true;
    s_init_call_count++;
    return BB_ERR_INVALID_ARG;
}

static bb_err_t fake_init_4(bb_http_handle_t server)
{
    (void)server;
    s_init_flags[3] = true;
    s_init_call_count++;
    return BB_OK;
}

// Foreach callback
typedef struct {
    const char *names[10];
    size_t count;
} foreach_ctx_t;

static void collect_names(const bb_init_entry_t *entry, void *ctx)
{
    foreach_ctx_t *fc = (foreach_ctx_t *)ctx;
    if (fc->count < 10) {
        fc->names[fc->count++] = entry->name;
    }
}

void test_bb_init_starts_empty(void)
{
    bb_init_clear();
    TEST_ASSERT_EQUAL(0, bb_init_count());
}

void test_bb_init_add_increments_count(void)
{
    bb_init_clear();
    bb_init_entry_t entry = { .name = "test1", .init = fake_init_1 };
    bb_init_add(&entry);
    TEST_ASSERT_EQUAL(1, bb_init_count());
}

void test_bb_init_foreach_visits_all_in_order(void)
{
    bb_init_clear();

    bb_init_entry_t e1 = { .name = "first", .init = fake_init_1 };
    bb_init_entry_t e2 = { .name = "second", .init = fake_init_2 };
    bb_init_entry_t e3 = { .name = "third", .init = fake_init_4 };

    bb_init_add(&e1);
    bb_init_add(&e2);
    bb_init_add(&e3);

    foreach_ctx_t ctx = { 0 };
    bb_init_foreach(collect_names, &ctx);

    TEST_ASSERT_EQUAL(3, ctx.count);
    TEST_ASSERT_EQUAL_STRING("first", ctx.names[0]);
    TEST_ASSERT_EQUAL_STRING("second", ctx.names[1]);
    TEST_ASSERT_EQUAL_STRING("third", ctx.names[2]);
}

void test_bb_init_init_calls_each_init_fn(void)
{
    bb_init_clear();
    memset(s_init_flags, 0, sizeof(s_init_flags));
    s_init_call_count = 0;

    bb_init_entry_t e1 = { .name = "e1", .init = fake_init_1 };
    bb_init_entry_t e2 = { .name = "e2", .init = fake_init_2 };
    bb_init_entry_t e3 = { .name = "e3", .init = fake_init_4 };

    bb_init_add(&e1);
    bb_init_add(&e2);
    bb_init_add(&e3);

    bb_err_t err = bb_init_init();

    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL(3, s_init_call_count);
    TEST_ASSERT_TRUE(s_init_flags[0]);
    TEST_ASSERT_TRUE(s_init_flags[1]);
    TEST_ASSERT_TRUE(s_init_flags[3]);
}

void test_bb_init_init_reports_first_error_but_continues(void)
{
    bb_init_clear();
    memset(s_init_flags, 0, sizeof(s_init_flags));
    s_init_call_count = 0;

    bb_init_entry_t e1 = { .name = "e1", .init = fake_init_1 };
    bb_init_entry_t e2 = { .name = "e2", .init = fake_init_3_error };
    bb_init_entry_t e3 = { .name = "e3", .init = fake_init_4 };

    bb_init_add(&e1);
    bb_init_add(&e2);
    bb_init_add(&e3);

    bb_err_t err = bb_init_init();

    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
    TEST_ASSERT_EQUAL(3, s_init_call_count);
    TEST_ASSERT_TRUE(s_init_flags[0]);
    TEST_ASSERT_TRUE(s_init_flags[2]);
    TEST_ASSERT_TRUE(s_init_flags[3]);
}

void test_bb_init_clear_resets_count(void)
{
    bb_init_clear();
    bb_init_entry_t entry = { .name = "test1", .init = fake_init_1 };
    bb_init_add(&entry);
    TEST_ASSERT_EQUAL(1, bb_init_count());

    bb_init_clear();
    TEST_ASSERT_EQUAL(0, bb_init_count());
}

// ============================================================================
// Order-priority tests
// ============================================================================

// Track invocation sequence for order tests
static int s_order_seq[10];
static int s_order_seq_idx = 0;

static bb_err_t order_fn_a(bb_http_handle_t server)
{
    (void)server;
    s_order_seq[s_order_seq_idx++] = 'A';
    return BB_OK;
}

static bb_err_t order_fn_b(bb_http_handle_t server)
{
    (void)server;
    s_order_seq[s_order_seq_idx++] = 'B';
    return BB_OK;
}

static bb_err_t order_fn_c(bb_http_handle_t server)
{
    (void)server;
    s_order_seq[s_order_seq_idx++] = 'C';
    return BB_OK;
}

static bb_err_t order_fn_d(bb_http_handle_t server)
{
    (void)server;
    s_order_seq[s_order_seq_idx++] = 'D';
    return BB_OK;
}

// Two entries registered in order A(order=5) then B(order=1): B must run first.
void test_bb_init_init_honors_order_priority(void)
{
    bb_init_clear();
    s_order_seq_idx = 0;

    bb_init_entry_t eA = { .name = "A", .init = order_fn_a, .order = 5 };
    bb_init_entry_t eB = { .name = "B", .init = order_fn_b, .order = 1 };

    bb_init_add(&eA);
    bb_init_add(&eB);

    bb_err_t err = bb_init_init();

    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL(2, s_order_seq_idx);
    // B (order=1) must run before A (order=5)
    TEST_ASSERT_EQUAL('B', s_order_seq[0]);
    TEST_ASSERT_EQUAL('A', s_order_seq[1]);
}

// Two entries with same order must run in registration (insertion) order — stable sort.
void test_bb_init_init_same_order_preserves_insertion_order(void)
{
    bb_init_clear();
    s_order_seq_idx = 0;

    bb_init_entry_t eC = { .name = "C", .init = order_fn_c, .order = 2 };
    bb_init_entry_t eD = { .name = "D", .init = order_fn_d, .order = 2 };

    bb_init_add(&eC);
    bb_init_add(&eD);

    bb_err_t err = bb_init_init();

    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL(2, s_order_seq_idx);
    // Same order: C registered first, must run first
    TEST_ASSERT_EQUAL('C', s_order_seq[0]);
    TEST_ASSERT_EQUAL('D', s_order_seq[1]);
}

// Mix: four entries, verify full ascending-order output with stable tie-breaking.
void test_bb_init_init_order_mixed(void)
{
    bb_init_clear();
    s_order_seq_idx = 0;

    // Register in this order: A(5), B(0), C(5), D(0)
    // Expected run order (ascending order, stable): B(0), D(0), A(5), C(5)
    bb_init_entry_t eA = { .name = "A", .init = order_fn_a, .order = 5 };
    bb_init_entry_t eB = { .name = "B", .init = order_fn_b, .order = 0 };
    bb_init_entry_t eC = { .name = "C", .init = order_fn_c, .order = 5 };
    bb_init_entry_t eD = { .name = "D", .init = order_fn_d, .order = 0 };

    bb_init_add(&eA);
    bb_init_add(&eB);
    bb_init_add(&eC);
    bb_init_add(&eD);

    bb_err_t err = bb_init_init();

    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL(4, s_order_seq_idx);
    TEST_ASSERT_EQUAL('B', s_order_seq[0]);
    TEST_ASSERT_EQUAL('D', s_order_seq[1]);
    TEST_ASSERT_EQUAL('A', s_order_seq[2]);
    TEST_ASSERT_EQUAL('C', s_order_seq[3]);
}

// ============================================================================
// PRE_HTTP tier tests
// ============================================================================

static bb_err_t pre_http_fn_1(void)
{
    s_pre_http_flags[0] = true;
    s_pre_http_order[s_pre_http_order_idx++] = 1;
    s_pre_http_call_count++;
    return BB_OK;
}

static bb_err_t pre_http_fn_2(void)
{
    s_pre_http_flags[1] = true;
    s_pre_http_order[s_pre_http_order_idx++] = 2;
    s_pre_http_call_count++;
    return BB_OK;
}

static bb_err_t pre_http_fn_3_error(void)
{
    s_pre_http_flags[2] = true;
    s_pre_http_order[s_pre_http_order_idx++] = 3;
    s_pre_http_call_count++;
    return BB_ERR_INVALID_ARG;
}

typedef struct {
    const char *names[10];
    size_t count;
} pre_http_foreach_ctx_t;

static void collect_pre_http_names(const bb_init_entry_pre_http_t *entry, void *ctx)
{
    pre_http_foreach_ctx_t *fc = (pre_http_foreach_ctx_t *)ctx;
    if (fc->count < 10) {
        fc->names[fc->count++] = entry->name;
    }
}

void test_bb_init_pre_http_starts_empty(void)
{
    bb_init_clear_pre_http();
    TEST_ASSERT_EQUAL(0, bb_init_count_pre_http());
}

void test_bb_init_pre_http_add_increments_count(void)
{
    bb_init_clear_pre_http();
    bb_init_entry_pre_http_t e = { .name = "ph1", .init = pre_http_fn_1 };
    bb_init_add_pre_http(&e);
    TEST_ASSERT_EQUAL(1, bb_init_count_pre_http());
}

void test_bb_init_pre_http_foreach_visits_in_insertion_order(void)
{
    bb_init_clear_pre_http();

    bb_init_entry_pre_http_t e1 = { .name = "first",  .init = pre_http_fn_1 };
    bb_init_entry_pre_http_t e2 = { .name = "second", .init = pre_http_fn_2 };

    bb_init_add_pre_http(&e1);
    bb_init_add_pre_http(&e2);

    pre_http_foreach_ctx_t ctx = { 0 };
    bb_init_foreach_pre_http(collect_pre_http_names, &ctx);

    TEST_ASSERT_EQUAL(2, ctx.count);
    TEST_ASSERT_EQUAL_STRING("first",  ctx.names[0]);
    TEST_ASSERT_EQUAL_STRING("second", ctx.names[1]);
}

void test_bb_init_pre_http_init_calls_each_fn(void)
{
    bb_init_clear_pre_http();
    memset(s_pre_http_flags, 0, sizeof(s_pre_http_flags));
    s_pre_http_call_count = 0;
    s_pre_http_order_idx = 0;

    bb_init_entry_pre_http_t e1 = { .name = "ph1", .init = pre_http_fn_1 };
    bb_init_entry_pre_http_t e2 = { .name = "ph2", .init = pre_http_fn_2 };

    bb_init_add_pre_http(&e1);
    bb_init_add_pre_http(&e2);

    bb_err_t err = bb_init_init_pre_http();

    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL(2, s_pre_http_call_count);
    TEST_ASSERT_TRUE(s_pre_http_flags[0]);
    TEST_ASSERT_TRUE(s_pre_http_flags[1]);
    // Insertion order: e1 first, e2 second
    TEST_ASSERT_EQUAL(1, s_pre_http_order[0]);
    TEST_ASSERT_EQUAL(2, s_pre_http_order[1]);
}

void test_bb_init_pre_http_init_reports_first_error_but_continues(void)
{
    bb_init_clear_pre_http();
    memset(s_pre_http_flags, 0, sizeof(s_pre_http_flags));
    s_pre_http_call_count = 0;
    s_pre_http_order_idx = 0;

    bb_init_entry_pre_http_t e1 = { .name = "ph1", .init = pre_http_fn_1   };
    bb_init_entry_pre_http_t e2 = { .name = "ph2", .init = pre_http_fn_3_error };
    bb_init_entry_pre_http_t e3 = { .name = "ph3", .init = pre_http_fn_2   };

    bb_init_add_pre_http(&e1);
    bb_init_add_pre_http(&e2);
    bb_init_add_pre_http(&e3);

    bb_err_t err = bb_init_init_pre_http();

    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
    TEST_ASSERT_EQUAL(3, s_pre_http_call_count);
    TEST_ASSERT_TRUE(s_pre_http_flags[0]);
    TEST_ASSERT_TRUE(s_pre_http_flags[1]);
    TEST_ASSERT_TRUE(s_pre_http_flags[2]);
}

void test_bb_init_pre_http_clear_resets_count(void)
{
    bb_init_clear_pre_http();
    bb_init_entry_pre_http_t e = { .name = "ph1", .init = pre_http_fn_1 };
    bb_init_add_pre_http(&e);
    TEST_ASSERT_EQUAL(1, bb_init_count_pre_http());

    bb_init_clear_pre_http();
    TEST_ASSERT_EQUAL(0, bb_init_count_pre_http());
}

// ============================================================================
// EARLY tier order-priority tests
// ============================================================================

static int s_early_order_seq[10];
static int s_early_order_seq_idx = 0;

static bb_err_t early_order_fn_a(void)
{
    s_early_order_seq[s_early_order_seq_idx++] = 'A';
    return BB_OK;
}

static bb_err_t early_order_fn_b(void)
{
    s_early_order_seq[s_early_order_seq_idx++] = 'B';
    return BB_OK;
}

static bb_err_t early_order_fn_c(void)
{
    s_early_order_seq[s_early_order_seq_idx++] = 'C';
    return BB_OK;
}

static bb_err_t early_order_fn_d(void)
{
    s_early_order_seq[s_early_order_seq_idx++] = 'D';
    return BB_OK;
}

// EARLY: distinct orders — lower order runs first regardless of registration order.
void test_bb_init_early_honors_order_priority(void)
{
    bb_init_clear_early();
    s_early_order_seq_idx = 0;

    bb_init_entry_early_t eA = { .name = "A", .init = early_order_fn_a, .order = 5 };
    bb_init_entry_early_t eB = { .name = "B", .init = early_order_fn_b, .order = 1 };

    bb_init_add_early(&eA);
    bb_init_add_early(&eB);

    bb_err_t err = bb_init_init_early();

    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL(2, s_early_order_seq_idx);
    TEST_ASSERT_EQUAL('B', s_early_order_seq[0]);
    TEST_ASSERT_EQUAL('A', s_early_order_seq[1]);
}

// EARLY: equal order preserves registration (insertion) order — stable sort.
void test_bb_init_early_same_order_preserves_insertion_order(void)
{
    bb_init_clear_early();
    s_early_order_seq_idx = 0;

    bb_init_entry_early_t eC = { .name = "C", .init = early_order_fn_c, .order = 2 };
    bb_init_entry_early_t eD = { .name = "D", .init = early_order_fn_d, .order = 2 };

    bb_init_add_early(&eC);
    bb_init_add_early(&eD);

    bb_err_t err = bb_init_init_early();

    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL(2, s_early_order_seq_idx);
    TEST_ASSERT_EQUAL('C', s_early_order_seq[0]);
    TEST_ASSERT_EQUAL('D', s_early_order_seq[1]);
}

// EARLY: mixed distinct + tied orders — full ascending order with stable tie-breaking.
void test_bb_init_early_order_mixed(void)
{
    bb_init_clear_early();
    s_early_order_seq_idx = 0;

    // Register in this order: A(5), B(0), C(5), D(0)
    // Expected run order (ascending order, stable): B(0), D(0), A(5), C(5)
    bb_init_entry_early_t eA = { .name = "A", .init = early_order_fn_a, .order = 5 };
    bb_init_entry_early_t eB = { .name = "B", .init = early_order_fn_b, .order = 0 };
    bb_init_entry_early_t eC = { .name = "C", .init = early_order_fn_c, .order = 5 };
    bb_init_entry_early_t eD = { .name = "D", .init = early_order_fn_d, .order = 0 };

    bb_init_add_early(&eA);
    bb_init_add_early(&eB);
    bb_init_add_early(&eC);
    bb_init_add_early(&eD);

    bb_err_t err = bb_init_init_early();

    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL(4, s_early_order_seq_idx);
    TEST_ASSERT_EQUAL('B', s_early_order_seq[0]);
    TEST_ASSERT_EQUAL('D', s_early_order_seq[1]);
    TEST_ASSERT_EQUAL('A', s_early_order_seq[2]);
    TEST_ASSERT_EQUAL('C', s_early_order_seq[3]);
}

// EARLY: default order (BB_INIT_REGISTER_EARLY wrapper registers order=0) — all
// order-0 entries still execute, in registration order (regression coverage for
// the unchanged plain macro).
void test_bb_init_early_default_order_zero_runs_in_registration_order(void)
{
    bb_init_clear_early();
    s_early_order_seq_idx = 0;

    bb_init_entry_early_t eA = { .name = "A", .init = early_order_fn_a, .order = 0 };
    bb_init_entry_early_t eB = { .name = "B", .init = early_order_fn_b, .order = 0 };

    bb_init_add_early(&eA);
    bb_init_add_early(&eB);

    bb_err_t err = bb_init_init_early();

    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL(2, s_early_order_seq_idx);
    TEST_ASSERT_EQUAL('A', s_early_order_seq[0]);
    TEST_ASSERT_EQUAL('B', s_early_order_seq[1]);
}

// ============================================================================
// PRE_HTTP tier order-priority tests
// ============================================================================

static int s_pre_http_order_seq[10];
static int s_pre_http_order_seq_idx = 0;

static bb_err_t pre_http_order_fn_a(void)
{
    s_pre_http_order_seq[s_pre_http_order_seq_idx++] = 'A';
    return BB_OK;
}

static bb_err_t pre_http_order_fn_b(void)
{
    s_pre_http_order_seq[s_pre_http_order_seq_idx++] = 'B';
    return BB_OK;
}

static bb_err_t pre_http_order_fn_c(void)
{
    s_pre_http_order_seq[s_pre_http_order_seq_idx++] = 'C';
    return BB_OK;
}

static bb_err_t pre_http_order_fn_d(void)
{
    s_pre_http_order_seq[s_pre_http_order_seq_idx++] = 'D';
    return BB_OK;
}

// PRE_HTTP: distinct orders — lower order runs first regardless of registration order.
void test_bb_init_pre_http_honors_order_priority(void)
{
    bb_init_clear_pre_http();
    s_pre_http_order_seq_idx = 0;

    bb_init_entry_pre_http_t eA = { .name = "A", .init = pre_http_order_fn_a, .order = 5 };
    bb_init_entry_pre_http_t eB = { .name = "B", .init = pre_http_order_fn_b, .order = 1 };

    bb_init_add_pre_http(&eA);
    bb_init_add_pre_http(&eB);

    bb_err_t err = bb_init_init_pre_http();

    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL(2, s_pre_http_order_seq_idx);
    TEST_ASSERT_EQUAL('B', s_pre_http_order_seq[0]);
    TEST_ASSERT_EQUAL('A', s_pre_http_order_seq[1]);
}

// PRE_HTTP: equal order preserves registration (insertion) order — stable sort.
void test_bb_init_pre_http_same_order_preserves_insertion_order(void)
{
    bb_init_clear_pre_http();
    s_pre_http_order_seq_idx = 0;

    bb_init_entry_pre_http_t eC = { .name = "C", .init = pre_http_order_fn_c, .order = 2 };
    bb_init_entry_pre_http_t eD = { .name = "D", .init = pre_http_order_fn_d, .order = 2 };

    bb_init_add_pre_http(&eC);
    bb_init_add_pre_http(&eD);

    bb_err_t err = bb_init_init_pre_http();

    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL(2, s_pre_http_order_seq_idx);
    TEST_ASSERT_EQUAL('C', s_pre_http_order_seq[0]);
    TEST_ASSERT_EQUAL('D', s_pre_http_order_seq[1]);
}

// PRE_HTTP: mixed distinct + tied orders — full ascending order with stable tie-breaking.
void test_bb_init_pre_http_order_mixed(void)
{
    bb_init_clear_pre_http();
    s_pre_http_order_seq_idx = 0;

    // Register in this order: A(5), B(0), C(5), D(0)
    // Expected run order (ascending order, stable): B(0), D(0), A(5), C(5)
    bb_init_entry_pre_http_t eA = { .name = "A", .init = pre_http_order_fn_a, .order = 5 };
    bb_init_entry_pre_http_t eB = { .name = "B", .init = pre_http_order_fn_b, .order = 0 };
    bb_init_entry_pre_http_t eC = { .name = "C", .init = pre_http_order_fn_c, .order = 5 };
    bb_init_entry_pre_http_t eD = { .name = "D", .init = pre_http_order_fn_d, .order = 0 };

    bb_init_add_pre_http(&eA);
    bb_init_add_pre_http(&eB);
    bb_init_add_pre_http(&eC);
    bb_init_add_pre_http(&eD);

    bb_err_t err = bb_init_init_pre_http();

    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL(4, s_pre_http_order_seq_idx);
    TEST_ASSERT_EQUAL('B', s_pre_http_order_seq[0]);
    TEST_ASSERT_EQUAL('D', s_pre_http_order_seq[1]);
    TEST_ASSERT_EQUAL('A', s_pre_http_order_seq[2]);
    TEST_ASSERT_EQUAL('C', s_pre_http_order_seq[3]);
}

// PRE_HTTP: default order (BB_INIT_REGISTER_PRE_HTTP wrapper registers order=0) —
// all order-0 entries still execute, in registration order (regression coverage
// for the unchanged plain macro).
void test_bb_init_pre_http_default_order_zero_runs_in_registration_order(void)
{
    bb_init_clear_pre_http();
    s_pre_http_order_seq_idx = 0;

    bb_init_entry_pre_http_t eA = { .name = "A", .init = pre_http_order_fn_a, .order = 0 };
    bb_init_entry_pre_http_t eB = { .name = "B", .init = pre_http_order_fn_b, .order = 0 };

    bb_init_add_pre_http(&eA);
    bb_init_add_pre_http(&eB);

    bb_err_t err = bb_init_init_pre_http();

    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL(2, s_pre_http_order_seq_idx);
    TEST_ASSERT_EQUAL('A', s_pre_http_order_seq[0]);
    TEST_ASSERT_EQUAL('B', s_pre_http_order_seq[1]);
}

// Idempotency guard: calling bb_init_init_pre_http() then bb_init_init()
// must not double-invoke PRE_HTTP inits — each entry runs exactly once.
void test_bb_init_pre_http_no_double_init_via_init(void)
{
    bb_init_clear_pre_http();
    bb_init_clear();
    memset(s_pre_http_flags, 0, sizeof(s_pre_http_flags));
    s_pre_http_call_count = 0;
    s_pre_http_order_idx  = 0;

    bb_init_entry_pre_http_t e1 = { .name = "ph1", .init = pre_http_fn_1 };
    bb_init_entry_pre_http_t e2 = { .name = "ph2", .init = pre_http_fn_2 };

    // Add a no-op regular entry so bb_init_init()'s regular walker has n > 0.
    bb_init_entry_t reg = { .name = "noop", .init = fake_init_1, .order = 0 };

    bb_init_add_pre_http(&e1);
    bb_init_add_pre_http(&e2);
    bb_init_add(&reg);

    // Consumer calls init_pre_http() standalone first, then init() — latent footgun scenario.
    bb_err_t err1 = bb_init_init_pre_http();
    TEST_ASSERT_EQUAL(BB_OK, err1);
    TEST_ASSERT_EQUAL(2, s_pre_http_call_count);

    // Reset regular-tier counter so it doesn't pollute PRE_HTTP count check.
    s_init_call_count = 0;

    // init() internally re-walks PRE_HTTP tier; guard must skip it.
    bb_err_t err2 = bb_init_init();
    TEST_ASSERT_EQUAL(BB_OK, err2);

    // Each PRE_HTTP init must have run exactly once total.
    TEST_ASSERT_EQUAL(2, s_pre_http_call_count);
    TEST_ASSERT_TRUE(s_pre_http_flags[0]);
    TEST_ASSERT_TRUE(s_pre_http_flags[1]);
}

// ============================================================================
// bb_init_init() embedded PRE_HTTP walk — exercises the sort inserted into
// bb_init_init()'s own PRE_HTTP block (distinct from the standalone
// bb_init_init_pre_http() entry point covered above), including the
// ascending-order swap, the tie-preserving branch, and the error-continue path.
// ============================================================================

void test_bb_init_init_sorts_pre_http_when_not_yet_walked(void)
{
    bb_init_clear_pre_http();
    bb_init_clear();
    memset(s_pre_http_flags, 0, sizeof(s_pre_http_flags));
    s_pre_http_call_count = 0;
    s_pre_http_order_seq_idx = 0;

    // Register in this order: A(5), B(1), C(1) — B/C tie at order=1, B first.
    bb_init_entry_pre_http_t eA = { .name = "A", .init = pre_http_order_fn_a, .order = 5 };
    bb_init_entry_pre_http_t eB = { .name = "B", .init = pre_http_order_fn_b, .order = 1 };
    bb_init_entry_pre_http_t eC = { .name = "C", .init = pre_http_order_fn_c, .order = 1 };

    bb_init_add_pre_http(&eA);
    bb_init_add_pre_http(&eB);
    bb_init_add_pre_http(&eC);

    // Add a no-op regular entry so bb_init_init()'s regular walker has n > 0.
    bb_init_entry_t reg = { .name = "noop", .init = fake_init_1, .order = 0 };
    bb_init_add(&reg);

    // s_pre_http_walked is false here (bb_init_clear_pre_http() reset it), so
    // bb_init_init() takes its own PRE_HTTP sort-and-walk branch rather than
    // deferring to a prior bb_init_init_pre_http() call.
    bb_err_t err = bb_init_init();

    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL(3, s_pre_http_order_seq_idx);
    TEST_ASSERT_EQUAL('B', s_pre_http_order_seq[0]);
    TEST_ASSERT_EQUAL('C', s_pre_http_order_seq[1]);
    TEST_ASSERT_EQUAL('A', s_pre_http_order_seq[2]);
}

void test_bb_init_init_sorts_pre_http_reports_error_when_not_yet_walked(void)
{
    bb_init_clear_pre_http();
    bb_init_clear();
    memset(s_pre_http_flags, 0, sizeof(s_pre_http_flags));
    s_pre_http_call_count = 0;
    s_pre_http_order_idx = 0;

    bb_init_entry_pre_http_t e1 = { .name = "ph1", .init = pre_http_fn_1,       .order = 0 };
    bb_init_entry_pre_http_t e2 = { .name = "ph2", .init = pre_http_fn_3_error, .order = 0 };

    bb_init_add_pre_http(&e1);
    bb_init_add_pre_http(&e2);

    bb_init_entry_t reg = { .name = "noop", .init = fake_init_1, .order = 0 };
    bb_init_add(&reg);

    bb_err_t err = bb_init_init();

    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
    TEST_ASSERT_EQUAL(2, s_pre_http_call_count);
}

// ============================================================================
// Defensive NULL-argument tests — pre-existing guard branches, added for
// completeness now that gcovr tracks platform/{host,espidf}/bb_init/.
// ============================================================================

void test_bb_init_add_null_entry_is_noop(void)
{
    bb_init_clear();
    bb_init_add(NULL);
    TEST_ASSERT_EQUAL(0, bb_init_count());
}

void test_bb_init_add_early_null_entry_is_noop(void)
{
    bb_init_clear_early();
    bb_init_add_early(NULL);
    TEST_ASSERT_EQUAL(0, bb_init_count_early());
}

void test_bb_init_add_pre_http_null_entry_is_noop(void)
{
    bb_init_clear_pre_http();
    bb_init_add_pre_http(NULL);
    TEST_ASSERT_EQUAL(0, bb_init_count_pre_http());
}

void test_bb_init_foreach_null_cb_is_noop(void)
{
    bb_init_clear();
    bb_init_entry_t e = { .name = "e", .init = fake_init_1 };
    bb_init_add(&e);
    bb_init_foreach(NULL, NULL); // must not crash
}

void test_bb_init_foreach_early_null_cb_is_noop(void)
{
    bb_init_clear_early();
    bb_init_entry_early_t e = { .name = "e", .init = pre_http_fn_1 };
    bb_init_add_early(&e);
    bb_init_foreach_early(NULL, NULL); // must not crash
}

void test_bb_init_foreach_pre_http_null_cb_is_noop(void)
{
    bb_init_clear_pre_http();
    bb_init_entry_pre_http_t e = { .name = "e", .init = pre_http_fn_1 };
    bb_init_add_pre_http(&e);
    bb_init_foreach_pre_http(NULL, NULL); // must not crash
}

// ============================================================================
// Empty-list foreach — exercises the zero-entry branch of each tier's
// collect/reverse-walk loop.
// ============================================================================

void test_bb_init_foreach_empty_visits_none(void)
{
    bb_init_clear();
    foreach_ctx_t ctx = { 0 };
    bb_init_foreach(collect_names, &ctx);
    TEST_ASSERT_EQUAL(0, ctx.count);
}

static void collect_early_names(const bb_init_entry_early_t *entry, void *ctx)
{
    foreach_ctx_t *fc = (foreach_ctx_t *)ctx;
    if (fc->count < 10) {
        fc->names[fc->count++] = entry->name;
    }
}

void test_bb_init_foreach_early_empty_visits_none(void)
{
    bb_init_clear_early();
    foreach_ctx_t ctx = { 0 };
    bb_init_foreach_early(collect_early_names, &ctx);
    TEST_ASSERT_EQUAL(0, ctx.count);
}

void test_bb_init_foreach_pre_http_empty_visits_none(void)
{
    bb_init_clear_pre_http();
    pre_http_foreach_ctx_t ctx = { 0 };
    bb_init_foreach_pre_http(collect_pre_http_names, &ctx);
    TEST_ASSERT_EQUAL(0, ctx.count);
}

// ============================================================================
// Second-error-does-not-overwrite-first-error branch — exercises the
// `first_error == BB_OK` half of `if (err != BB_OK && first_error == BB_OK)`
// in every walker's error-continue check, which a single-error test cannot
// reach (that half is only false once first_error is already set).
// ============================================================================

static bb_err_t fake_init_5_error(bb_http_handle_t server)
{
    (void)server;
    s_init_call_count++;
    return BB_ERR_TIMEOUT;
}

void test_bb_init_init_regular_second_error_does_not_overwrite_first(void)
{
    bb_init_clear();
    s_init_call_count = 0;

    // Isolate from the PRE_HTTP tier: bb_init_init() delegates to
    // bb_init_init_pre_http() (KB #692), so a dirty/leftover PRE_HTTP
    // registry from a prior test could set first_error before the regular
    // walk below even runs, masking the "second regular-tier error does not
    // overwrite first" branch this test targets.
    bb_init_clear_pre_http();
    TEST_ASSERT_EQUAL(BB_OK, bb_init_init_pre_http()); // walked+empty -> no-op on the delegated call inside bb_init_init()

    bb_init_entry_t e1 = { .name = "e1", .init = fake_init_3_error };
    bb_init_entry_t e2 = { .name = "e2", .init = fake_init_5_error };

    bb_init_add(&e1);
    bb_init_add(&e2);

    bb_err_t err = bb_init_init();

    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err); // first error wins, not overwritten
    TEST_ASSERT_EQUAL(2, s_init_call_count);
}

static bb_err_t early_fn_error_1(void)
{
    return BB_ERR_INVALID_ARG;
}

static bb_err_t early_fn_error_2(void)
{
    return BB_ERR_TIMEOUT;
}

void test_bb_init_early_second_error_does_not_overwrite_first(void)
{
    bb_init_clear_early();

    bb_init_entry_early_t e1 = { .name = "e1", .init = early_fn_error_1 };
    bb_init_entry_early_t e2 = { .name = "e2", .init = early_fn_error_2 };

    bb_init_add_early(&e1);
    bb_init_add_early(&e2);

    bb_err_t err = bb_init_init_early();

    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
}

void test_bb_init_pre_http_standalone_second_error_does_not_overwrite_first(void)
{
    bb_init_clear_pre_http();

    bb_init_entry_pre_http_t e1 = { .name = "e1", .init = early_fn_error_1 };
    bb_init_entry_pre_http_t e2 = { .name = "e2", .init = early_fn_error_2 };

    bb_init_add_pre_http(&e1);
    bb_init_add_pre_http(&e2);

    bb_err_t err = bb_init_init_pre_http();

    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
}

void test_bb_init_init_pre_http_embedded_second_error_does_not_overwrite_first(void)
{
    bb_init_clear_pre_http();
    bb_init_clear();

    bb_init_entry_pre_http_t e1 = { .name = "e1", .init = early_fn_error_1 };
    bb_init_entry_pre_http_t e2 = { .name = "e2", .init = early_fn_error_2 };
    bb_init_add_pre_http(&e1);
    bb_init_add_pre_http(&e2);

    bb_init_entry_t reg = { .name = "noop", .init = fake_init_1, .order = 0 };
    bb_init_add(&reg);

    // s_pre_http_walked is false (bb_init_clear_pre_http() reset it), so
    // bb_init_init() runs its own embedded PRE_HTTP sort-and-walk block.
    bb_err_t err = bb_init_init();

    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
}

// ============================================================================
// 256-entry collector cap — each tier's collector loop is bounded at 256
// (`p && n < 256`); registering more than 256 entries exercises the cap's
// false branch, which no other test reaches.
// ============================================================================

static bb_err_t cap_noop_regular(bb_http_handle_t server) { (void)server; return BB_OK; }
static bb_err_t cap_noop_early(void) { return BB_OK; }
static bb_err_t cap_noop_pre_http(void) { return BB_OK; }

static bb_init_entry_t s_cap_regular_entries[300];
static bb_init_entry_early_t s_cap_early_entries[300];
static bb_init_entry_pre_http_t s_cap_pre_http_entries[300];

void test_bb_init_init_collector_cap_at_256_regular_entries(void)
{
    bb_init_clear();
    for (int i = 0; i < 300; i++) {
        s_cap_regular_entries[i] = (bb_init_entry_t){ .name = "cap", .init = cap_noop_regular, .order = 0 };
        bb_init_add(&s_cap_regular_entries[i]);
    }
    TEST_ASSERT_EQUAL(300, bb_init_count());

    bb_err_t err = bb_init_init();
    TEST_ASSERT_EQUAL(BB_OK, err);

    foreach_ctx_t ctx = { 0 };
    bb_init_foreach(collect_names, &ctx);
    TEST_ASSERT_EQUAL(10, ctx.count); // collect_names itself caps at 10 slots

    bb_init_clear();
}

void test_bb_init_early_collector_cap_at_256_entries(void)
{
    bb_init_clear_early();
    for (int i = 0; i < 300; i++) {
        s_cap_early_entries[i] = (bb_init_entry_early_t){ .name = "cap", .init = cap_noop_early, .order = 0 };
        bb_init_add_early(&s_cap_early_entries[i]);
    }
    TEST_ASSERT_EQUAL(300, bb_init_count_early());

    bb_err_t err = bb_init_init_early();
    TEST_ASSERT_EQUAL(BB_OK, err);

    foreach_ctx_t ctx = { 0 };
    bb_init_foreach_early(collect_early_names, &ctx);
    TEST_ASSERT_EQUAL(10, ctx.count);

    bb_init_clear_early();
}

void test_bb_init_pre_http_collector_cap_at_256_entries_standalone(void)
{
    bb_init_clear_pre_http();
    for (int i = 0; i < 300; i++) {
        s_cap_pre_http_entries[i] = (bb_init_entry_pre_http_t){ .name = "cap", .init = cap_noop_pre_http, .order = 0 };
        bb_init_add_pre_http(&s_cap_pre_http_entries[i]);
    }
    TEST_ASSERT_EQUAL(300, bb_init_count_pre_http());

    bb_err_t err = bb_init_init_pre_http();
    TEST_ASSERT_EQUAL(BB_OK, err);

    pre_http_foreach_ctx_t ctx = { 0 };
    bb_init_foreach_pre_http(collect_pre_http_names, &ctx);
    TEST_ASSERT_EQUAL(10, ctx.count);

    bb_init_clear_pre_http();
}

void test_bb_init_init_collector_cap_at_256_pre_http_entries_embedded(void)
{
    bb_init_clear_pre_http();
    bb_init_clear();
    for (int i = 0; i < 300; i++) {
        s_cap_pre_http_entries[i] = (bb_init_entry_pre_http_t){ .name = "cap", .init = cap_noop_pre_http, .order = 0 };
        bb_init_add_pre_http(&s_cap_pre_http_entries[i]);
    }

    bb_init_entry_t reg = { .name = "noop", .init = fake_init_1, .order = 0 };
    bb_init_add(&reg);

    // s_pre_http_walked is false here, so bb_init_init() walks its own
    // embedded PRE_HTTP collector/sort block with > 256 entries queued.
    bb_err_t err = bb_init_init();
    TEST_ASSERT_EQUAL(BB_OK, err);

    bb_init_clear_pre_http();
}

// Calling bb_init_init_pre_http() twice in a row (standalone, not via
// bb_init_init()) must skip the second walk via its own s_pre_http_walked
// guard and return BB_OK immediately without re-invoking any init fn.
void test_bb_init_pre_http_standalone_double_call_is_noop_second_time(void)
{
    bb_init_clear_pre_http();
    memset(s_pre_http_flags, 0, sizeof(s_pre_http_flags));
    s_pre_http_call_count = 0;
    s_pre_http_order_idx  = 0;

    bb_init_entry_pre_http_t e1 = { .name = "ph1", .init = pre_http_fn_1 };
    bb_init_add_pre_http(&e1);

    bb_err_t err1 = bb_init_init_pre_http();
    TEST_ASSERT_EQUAL(BB_OK, err1);
    TEST_ASSERT_EQUAL(1, s_pre_http_call_count);

    bb_err_t err2 = bb_init_init_pre_http();
    TEST_ASSERT_EQUAL(BB_OK, err2);
    TEST_ASSERT_EQUAL(1, s_pre_http_call_count); // not re-invoked
}

// ============================================================================
// malloc-NULL branch coverage — each tier's bb_init_add* function must no-op
// (not add the entry) when the underlying allocator returns NULL. Host bb_init
// uses bb_malloc_prefer_spiram, which honors bb_mem_set_alloc_hook under
// BB_MEM_TESTING, making these paths injectable (mirrors test_bb_sub.c's
// s_always_fail_malloc pattern). Hook is reset after each test so the failure
// injection never leaks into other tests.
// ============================================================================

#ifdef BB_MEM_TESTING
static void *s_init_always_fail_malloc(size_t sz)
{
    (void)sz;
    return NULL;
}
#endif

void test_bb_init_add_regular_malloc_null_is_noop(void)
{
#ifdef BB_MEM_TESTING
    bb_init_clear();

    bb_init_entry_t e = { .name = "fail1", .init = fake_init_1 };
    bb_mem_set_alloc_hook(s_init_always_fail_malloc);
    bb_init_add(&e);
    bb_mem_set_alloc_hook(NULL);

    TEST_ASSERT_EQUAL(0, bb_init_count());
#else
    TEST_IGNORE_MESSAGE("BB_MEM_TESTING not enabled");
#endif
}

void test_bb_init_add_early_malloc_null_is_noop(void)
{
#ifdef BB_MEM_TESTING
    bb_init_clear_early();

    bb_init_entry_early_t e = { .name = "fail1", .init = pre_http_fn_1 };
    bb_mem_set_alloc_hook(s_init_always_fail_malloc);
    bb_init_add_early(&e);
    bb_mem_set_alloc_hook(NULL);

    TEST_ASSERT_EQUAL(0, bb_init_count_early());
#else
    TEST_IGNORE_MESSAGE("BB_MEM_TESTING not enabled");
#endif
}

// ============================================================================
// Empty-registry (n==0) walker tests — exercise the extracted
// bb_init_sort_nodes_by_order() guard (`if (n < 2) return;`), which replaced
// the old per-site `hi = n - 1` reverse loop that underflowed to SIZE_MAX for
// n==0 (OOB stack access). One test per tier's init walker.
// ============================================================================

// EARLY: standalone walker with zero entries must return BB_OK without
// touching the (empty) nodes array.
void test_bb_init_early_init_empty_is_noop(void)
{
    bb_init_clear_early();

    bb_err_t err = bb_init_init_early();

    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL(0, bb_init_count_early());
}

// PRE_HTTP: standalone walker with zero entries must return BB_OK without
// touching the (empty) nodes array.
void test_bb_init_pre_http_init_empty_is_noop(void)
{
    bb_init_clear_pre_http();

    bb_err_t err = bb_init_init_pre_http();

    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL(0, bb_init_count_pre_http());
}

// REGULAR + embedded PRE_HTTP: bb_init_init() with both tiers empty exercises
// the n==0 guard in both the embedded PRE_HTTP block and the REGULAR block in
// a single call (the REGULAR site is the one that previously underflowed).
void test_bb_init_init_all_empty_is_noop(void)
{
    bb_init_clear_pre_http();
    bb_init_clear();

    bb_err_t err = bb_init_init();

    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL(0, bb_init_count());
    TEST_ASSERT_EQUAL(0, bb_init_count_pre_http());
}

void test_bb_init_add_pre_http_malloc_null_is_noop(void)
{
#ifdef BB_MEM_TESTING
    bb_init_clear_pre_http();

    bb_init_entry_pre_http_t e = { .name = "fail1", .init = pre_http_fn_1 };
    bb_mem_set_alloc_hook(s_init_always_fail_malloc);
    bb_init_add_pre_http(&e);
    bb_mem_set_alloc_hook(NULL);

    TEST_ASSERT_EQUAL(0, bb_init_count_pre_http());
#else
    TEST_IGNORE_MESSAGE("BB_MEM_TESTING not enabled");
#endif
}
