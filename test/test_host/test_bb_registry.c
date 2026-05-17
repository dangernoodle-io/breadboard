#include "unity.h"
#include "bb_registry.h"
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

static void collect_names(const bb_registry_entry_t *entry, void *ctx)
{
    foreach_ctx_t *fc = (foreach_ctx_t *)ctx;
    if (fc->count < 10) {
        fc->names[fc->count++] = entry->name;
    }
}

void test_bb_registry_starts_empty(void)
{
    bb_registry_clear();
    TEST_ASSERT_EQUAL(0, bb_registry_count());
}

void test_bb_registry_add_increments_count(void)
{
    bb_registry_clear();
    bb_registry_entry_t entry = { .name = "test1", .init = fake_init_1 };
    bb_registry_add(&entry);
    TEST_ASSERT_EQUAL(1, bb_registry_count());
}

void test_bb_registry_foreach_visits_all_in_order(void)
{
    bb_registry_clear();

    bb_registry_entry_t e1 = { .name = "first", .init = fake_init_1 };
    bb_registry_entry_t e2 = { .name = "second", .init = fake_init_2 };
    bb_registry_entry_t e3 = { .name = "third", .init = fake_init_4 };

    bb_registry_add(&e1);
    bb_registry_add(&e2);
    bb_registry_add(&e3);

    foreach_ctx_t ctx = { 0 };
    bb_registry_foreach(collect_names, &ctx);

    TEST_ASSERT_EQUAL(3, ctx.count);
    TEST_ASSERT_EQUAL_STRING("first", ctx.names[0]);
    TEST_ASSERT_EQUAL_STRING("second", ctx.names[1]);
    TEST_ASSERT_EQUAL_STRING("third", ctx.names[2]);
}

void test_bb_registry_init_calls_each_init_fn(void)
{
    bb_registry_clear();
    memset(s_init_flags, 0, sizeof(s_init_flags));
    s_init_call_count = 0;

    bb_registry_entry_t e1 = { .name = "e1", .init = fake_init_1 };
    bb_registry_entry_t e2 = { .name = "e2", .init = fake_init_2 };
    bb_registry_entry_t e3 = { .name = "e3", .init = fake_init_4 };

    bb_registry_add(&e1);
    bb_registry_add(&e2);
    bb_registry_add(&e3);

    bb_err_t err = bb_registry_init();

    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL(3, s_init_call_count);
    TEST_ASSERT_TRUE(s_init_flags[0]);
    TEST_ASSERT_TRUE(s_init_flags[1]);
    TEST_ASSERT_TRUE(s_init_flags[3]);
}

void test_bb_registry_init_reports_first_error_but_continues(void)
{
    bb_registry_clear();
    memset(s_init_flags, 0, sizeof(s_init_flags));
    s_init_call_count = 0;

    bb_registry_entry_t e1 = { .name = "e1", .init = fake_init_1 };
    bb_registry_entry_t e2 = { .name = "e2", .init = fake_init_3_error };
    bb_registry_entry_t e3 = { .name = "e3", .init = fake_init_4 };

    bb_registry_add(&e1);
    bb_registry_add(&e2);
    bb_registry_add(&e3);

    bb_err_t err = bb_registry_init();

    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
    TEST_ASSERT_EQUAL(3, s_init_call_count);
    TEST_ASSERT_TRUE(s_init_flags[0]);
    TEST_ASSERT_TRUE(s_init_flags[2]);
    TEST_ASSERT_TRUE(s_init_flags[3]);
}

void test_bb_registry_clear_resets_count(void)
{
    bb_registry_clear();
    bb_registry_entry_t entry = { .name = "test1", .init = fake_init_1 };
    bb_registry_add(&entry);
    TEST_ASSERT_EQUAL(1, bb_registry_count());

    bb_registry_clear();
    TEST_ASSERT_EQUAL(0, bb_registry_count());
}

void test_bb_registry_route_count_total_empty(void)
{
    bb_registry_clear();
    TEST_ASSERT_EQUAL(0, bb_registry_route_count_total());
}

void test_bb_registry_route_count_total_sums_correctly(void)
{
    bb_registry_clear();
    bb_registry_entry_t e1 = { .name = "e1", .init = fake_init_1, .route_count = 3 };
    bb_registry_entry_t e2 = { .name = "e2", .init = fake_init_2, .route_count = 5 };
    bb_registry_entry_t e3 = { .name = "e3", .init = fake_init_4, .route_count = 0 };

    bb_registry_add(&e1);
    bb_registry_add(&e2);
    bb_registry_add(&e3);

    TEST_ASSERT_EQUAL(8, bb_registry_route_count_total());
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

static void collect_pre_http_names(const bb_registry_entry_pre_http_t *entry, void *ctx)
{
    pre_http_foreach_ctx_t *fc = (pre_http_foreach_ctx_t *)ctx;
    if (fc->count < 10) {
        fc->names[fc->count++] = entry->name;
    }
}

void test_bb_registry_pre_http_starts_empty(void)
{
    bb_registry_clear_pre_http();
    TEST_ASSERT_EQUAL(0, bb_registry_count_pre_http());
}

void test_bb_registry_pre_http_add_increments_count(void)
{
    bb_registry_clear_pre_http();
    bb_registry_entry_pre_http_t e = { .name = "ph1", .init = pre_http_fn_1 };
    bb_registry_add_pre_http(&e);
    TEST_ASSERT_EQUAL(1, bb_registry_count_pre_http());
}

void test_bb_registry_pre_http_foreach_visits_in_insertion_order(void)
{
    bb_registry_clear_pre_http();

    bb_registry_entry_pre_http_t e1 = { .name = "first",  .init = pre_http_fn_1 };
    bb_registry_entry_pre_http_t e2 = { .name = "second", .init = pre_http_fn_2 };

    bb_registry_add_pre_http(&e1);
    bb_registry_add_pre_http(&e2);

    pre_http_foreach_ctx_t ctx = { 0 };
    bb_registry_foreach_pre_http(collect_pre_http_names, &ctx);

    TEST_ASSERT_EQUAL(2, ctx.count);
    TEST_ASSERT_EQUAL_STRING("first",  ctx.names[0]);
    TEST_ASSERT_EQUAL_STRING("second", ctx.names[1]);
}

void test_bb_registry_pre_http_init_calls_each_fn(void)
{
    bb_registry_clear_pre_http();
    memset(s_pre_http_flags, 0, sizeof(s_pre_http_flags));
    s_pre_http_call_count = 0;
    s_pre_http_order_idx = 0;

    bb_registry_entry_pre_http_t e1 = { .name = "ph1", .init = pre_http_fn_1 };
    bb_registry_entry_pre_http_t e2 = { .name = "ph2", .init = pre_http_fn_2 };

    bb_registry_add_pre_http(&e1);
    bb_registry_add_pre_http(&e2);

    bb_err_t err = bb_registry_init_pre_http();

    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL(2, s_pre_http_call_count);
    TEST_ASSERT_TRUE(s_pre_http_flags[0]);
    TEST_ASSERT_TRUE(s_pre_http_flags[1]);
    // Insertion order: e1 first, e2 second
    TEST_ASSERT_EQUAL(1, s_pre_http_order[0]);
    TEST_ASSERT_EQUAL(2, s_pre_http_order[1]);
}

void test_bb_registry_pre_http_init_reports_first_error_but_continues(void)
{
    bb_registry_clear_pre_http();
    memset(s_pre_http_flags, 0, sizeof(s_pre_http_flags));
    s_pre_http_call_count = 0;
    s_pre_http_order_idx = 0;

    bb_registry_entry_pre_http_t e1 = { .name = "ph1", .init = pre_http_fn_1   };
    bb_registry_entry_pre_http_t e2 = { .name = "ph2", .init = pre_http_fn_3_error };
    bb_registry_entry_pre_http_t e3 = { .name = "ph3", .init = pre_http_fn_2   };

    bb_registry_add_pre_http(&e1);
    bb_registry_add_pre_http(&e2);
    bb_registry_add_pre_http(&e3);

    bb_err_t err = bb_registry_init_pre_http();

    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
    TEST_ASSERT_EQUAL(3, s_pre_http_call_count);
    TEST_ASSERT_TRUE(s_pre_http_flags[0]);
    TEST_ASSERT_TRUE(s_pre_http_flags[1]);
    TEST_ASSERT_TRUE(s_pre_http_flags[2]);
}

void test_bb_registry_pre_http_clear_resets_count(void)
{
    bb_registry_clear_pre_http();
    bb_registry_entry_pre_http_t e = { .name = "ph1", .init = pre_http_fn_1 };
    bb_registry_add_pre_http(&e);
    TEST_ASSERT_EQUAL(1, bb_registry_count_pre_http());

    bb_registry_clear_pre_http();
    TEST_ASSERT_EQUAL(0, bb_registry_count_pre_http());
}
