// Tests for bb_pub subscription filter and per-source tags.
#include "unity.h"
#include "bb_pub.h"
#include "bb_nv.h"

#include <string.h>

// ---------------------------------------------------------------------------
// Sample helpers
// ---------------------------------------------------------------------------

static bool sample_val(bb_json_t obj, void *ctx)
{
    (void)ctx;
    bb_json_obj_set_number(obj, "val", 1.0);
    return true;
}

// ---------------------------------------------------------------------------
// Capture helpers
// ---------------------------------------------------------------------------

static int s_publish_count = 0;
static char s_last_subtopic[64];

static bb_err_t spy_publish(void *ctx, const char *topic, const char *payload,
                             int len, bool retain)
{
    (void)ctx;
    (void)payload;
    (void)len;
    (void)retain;
    s_publish_count++;
    // Extract subtopic (after 2nd slash).
    const char *p = topic;
    int slashes = 0;
    while (*p && slashes < 2) { if (*p == '/') slashes++; p++; }
    strncpy(s_last_subtopic, p, sizeof(s_last_subtopic) - 1);
    s_last_subtopic[sizeof(s_last_subtopic) - 1] = '\0';
    return BB_OK;
}

static void reset_spy(void)
{
    s_publish_count = 0;
    s_last_subtopic[0] = '\0';
}

static void setup_test(void)
{
    // Global setUp already calls bb_pub_test_reset().
    reset_spy();
    bb_nv_config_set_hostname("testhost");
}

// ---------------------------------------------------------------------------
// subscription_match tests
// ---------------------------------------------------------------------------

void test_bb_pub_subscription_match_null_sub_always_true(void)
{
    TEST_ASSERT_TRUE(bb_pub_subscription_match(NULL, "power", NULL, 0));
}

void test_bb_pub_subscription_match_both_null_lists_always_true(void)
{
    bb_pub_subscription_t sub = { .subtopics = NULL, .nsubtopics = 0,
                                  .tags = NULL, .ntags = 0 };
    TEST_ASSERT_TRUE(bb_pub_subscription_match(&sub, "power", NULL, 0));
}

void test_bb_pub_subscription_match_subtopic_hit(void)
{
    const char *st[] = { "power", "fan" };
    bb_pub_subscription_t sub = { .subtopics = st, .nsubtopics = 2,
                                  .tags = NULL, .ntags = 0 };
    TEST_ASSERT_TRUE(bb_pub_subscription_match(&sub, "power", NULL, 0));
}

void test_bb_pub_subscription_match_subtopic_miss(void)
{
    const char *st[] = { "power" };
    bb_pub_subscription_t sub = { .subtopics = st, .nsubtopics = 1,
                                  .tags = NULL, .ntags = 0 };
    TEST_ASSERT_FALSE(bb_pub_subscription_match(&sub, "wifi", NULL, 0));
}

void test_bb_pub_subscription_match_tag_hit(void)
{
    const char *allowed_tags[] = { "mining" };
    bb_pub_subscription_t sub = { .subtopics = NULL, .nsubtopics = 0,
                                  .tags = allowed_tags, .ntags = 1 };
    const char *src_tags[] = { "mining", "telemetry" };
    TEST_ASSERT_TRUE(bb_pub_subscription_match(&sub, "power", src_tags, 2));
}

void test_bb_pub_subscription_match_tag_miss(void)
{
    const char *allowed_tags[] = { "mining" };
    bb_pub_subscription_t sub = { .subtopics = NULL, .nsubtopics = 0,
                                  .tags = allowed_tags, .ntags = 1 };
    const char *src_tags[] = { "telemetry" };
    TEST_ASSERT_FALSE(bb_pub_subscription_match(&sub, "power", src_tags, 1));
}

void test_bb_pub_subscription_match_or_logic_subtopic_matches(void)
{
    const char *st[] = { "power" };
    const char *allowed_tags[] = { "mining" };
    bb_pub_subscription_t sub = { .subtopics = st, .nsubtopics = 1,
                                  .tags = allowed_tags, .ntags = 1 };
    // subtopic matches, no tags — should be true
    TEST_ASSERT_TRUE(bb_pub_subscription_match(&sub, "power", NULL, 0));
}

void test_bb_pub_subscription_match_or_logic_tag_matches(void)
{
    const char *st[] = { "power" };
    const char *allowed_tags[] = { "mining" };
    bb_pub_subscription_t sub = { .subtopics = st, .nsubtopics = 1,
                                  .tags = allowed_tags, .ntags = 1 };
    const char *src_tags[] = { "mining" };
    // tag matches even though subtopic doesn't
    TEST_ASSERT_TRUE(bb_pub_subscription_match(&sub, "wifi", src_tags, 1));
}

void test_bb_pub_subscription_match_or_logic_neither_matches(void)
{
    const char *st[] = { "power" };
    const char *allowed_tags[] = { "mining" };
    bb_pub_subscription_t sub = { .subtopics = st, .nsubtopics = 1,
                                  .tags = allowed_tags, .ntags = 1 };
    const char *src_tags[] = { "telemetry" };
    TEST_ASSERT_FALSE(bb_pub_subscription_match(&sub, "wifi", src_tags, 1));
}

// ---------------------------------------------------------------------------
// register_source_ex / source_info_ex tests
// ---------------------------------------------------------------------------

void test_bb_pub_register_source_ex_no_tags(void)
{
    TEST_ASSERT_EQUAL(BB_OK, bb_pub_register_source_ex("power", sample_val, NULL, NULL, 0));
    int ntags = -1;
    const char *const *tags = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_pub_source_info_ex(0, NULL, NULL, NULL, NULL, NULL, &tags, &ntags));
    TEST_ASSERT_EQUAL(0, ntags);
}

void test_bb_pub_register_source_ex_with_tags(void)
{
    const char *tags_in[] = { "mining", "power" };
    TEST_ASSERT_EQUAL(BB_OK, bb_pub_register_source_ex("power", sample_val, NULL, tags_in, 2));
    int ntags = 0;
    const char *const *tags = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_pub_source_info_ex(0, NULL, NULL, NULL, NULL, NULL, &tags, &ntags));
    TEST_ASSERT_EQUAL(2, ntags);
    TEST_ASSERT_EQUAL_STRING("mining", tags[0]);
    TEST_ASSERT_EQUAL_STRING("power", tags[1]);
}

void test_bb_pub_register_source_ex_tags_capped(void)
{
    const char *tags_in[BB_PUB_MAX_TAGS_PER_SOURCE + 2];
    for (int i = 0; i < BB_PUB_MAX_TAGS_PER_SOURCE + 2; i++) tags_in[i] = "t";
    TEST_ASSERT_EQUAL(BB_OK,
        bb_pub_register_source_ex("power", sample_val, NULL,
                                  tags_in, BB_PUB_MAX_TAGS_PER_SOURCE + 2));
    int ntags = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_pub_source_info_ex(0, NULL, NULL, NULL, NULL, NULL, NULL, &ntags));
    TEST_ASSERT_EQUAL(BB_PUB_MAX_TAGS_PER_SOURCE, ntags);
}

void test_bb_pub_source_info_ex_out_of_range(void)
{
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_pub_source_info_ex(0, NULL, NULL, NULL, NULL, NULL, NULL, NULL));
}

// ---------------------------------------------------------------------------
// subscription filter applied in tick_once
// ---------------------------------------------------------------------------

void test_bb_pub_subscribe_fn_null_receives_all(void)
{
    setup_test();
    bb_pub_sink_t sink = { .publish = spy_publish, .subscribe = NULL };
    bb_pub_set_sink(&sink);
    bb_pub_register_source("power", sample_val, NULL);
    bb_pub_register_source("fan", sample_val, NULL);
    bb_pub_tick_once();
    TEST_ASSERT_EQUAL(2, s_publish_count);
}

void test_bb_pub_subscribe_fn_filters_by_subtopic(void)
{
    setup_test();
    static const char *allowed[] = { "power" };
    static bb_pub_subscription_t sub = { .subtopics = allowed, .nsubtopics = 1,
                                         .tags = NULL, .ntags = 0 };
    bb_pub_sink_t sink = { .publish = spy_publish,
                           .subscribe = bb_pub_subscription_predicate,
                           .subscribe_ctx = &sub };
    bb_pub_set_sink(&sink);
    bb_pub_register_source("power", sample_val, NULL);
    bb_pub_register_source("fan", sample_val, NULL);
    bb_pub_tick_once();
    TEST_ASSERT_EQUAL(1, s_publish_count);
    TEST_ASSERT_EQUAL_STRING("power", s_last_subtopic);
}

void test_bb_pub_subscribe_fn_filters_by_tag(void)
{
    setup_test();
    static const char *allowed_tags[] = { "mining" };
    static bb_pub_subscription_t sub = { .subtopics = NULL, .nsubtopics = 0,
                                         .tags = allowed_tags, .ntags = 1 };
    bb_pub_sink_t sink = { .publish = spy_publish,
                           .subscribe = bb_pub_subscription_predicate,
                           .subscribe_ctx = &sub };
    bb_pub_set_sink(&sink);
    const char *tags_mining[] = { "mining" };
    const char *tags_none[] = { "telemetry" };
    bb_pub_register_source_ex("power", sample_val, NULL, tags_mining, 1);
    bb_pub_register_source_ex("fan",   sample_val, NULL, tags_none,   1);
    bb_pub_tick_once();
    TEST_ASSERT_EQUAL(1, s_publish_count);
    TEST_ASSERT_EQUAL_STRING("power", s_last_subtopic);
}

void test_bb_pub_subscribe_fn_all_filtered_no_publish(void)
{
    setup_test();
    static const char *allowed[] = { "nope" };
    static bb_pub_subscription_t sub = { .subtopics = allowed, .nsubtopics = 1,
                                         .tags = NULL, .ntags = 0 };
    bb_pub_sink_t sink = { .publish = spy_publish,
                           .subscribe = bb_pub_subscription_predicate,
                           .subscribe_ctx = &sub };
    bb_pub_set_sink(&sink);
    bb_pub_register_source("power", sample_val, NULL);
    bb_pub_tick_once();
    TEST_ASSERT_EQUAL(0, s_publish_count);
}

// ---------------------------------------------------------------------------
// sample_into tests
// ---------------------------------------------------------------------------

void test_bb_pub_sample_into_returns_false_no_source(void)
{
    bb_json_t obj = bb_json_obj_new();
    TEST_ASSERT_FALSE(bb_pub_sample_into("power", obj));
    bb_json_free(obj);
}

void test_bb_pub_sample_into_calls_source(void)
{
    bb_pub_register_source("power", sample_val, NULL);
    bb_json_t obj = bb_json_obj_new();
    TEST_ASSERT_TRUE(bb_pub_sample_into("power", obj));
    double val = 0.0;
    TEST_ASSERT_TRUE(bb_json_obj_get_number(obj, "val", &val));
    TEST_ASSERT_EQUAL_DOUBLE(1.0, val);
    bb_json_free(obj);
}

void test_bb_pub_sample_into_null_subtopic(void)
{
    bb_json_t obj = bb_json_obj_new();
    TEST_ASSERT_FALSE(bb_pub_sample_into(NULL, obj));
    bb_json_free(obj);
}

void test_bb_pub_sample_into_null_obj(void)
{
    bb_pub_register_source("power", sample_val, NULL);
    TEST_ASSERT_FALSE(bb_pub_sample_into("power", NULL));
}

// bb_pub_sample_into: with multiple sources, a non-matching earlier entry
// must be skipped (continue) before the matching one is found.
void test_bb_pub_sample_into_skips_non_matching_sources_first(void)
{
    bb_pub_register_source("wifi", sample_val, NULL);
    bb_pub_register_source("power", sample_val, NULL);
    bb_json_t obj = bb_json_obj_new();
    TEST_ASSERT_TRUE(bb_pub_sample_into("power", obj));
    bb_json_free(obj);
}

// bb_pub_subscription_match: a NULL entry inside the subtopics array must
// be skipped safely rather than dereferenced.
void test_bb_pub_subscription_match_null_entry_in_subtopics_skipped(void)
{
    const char *list[] = { NULL, "power" };
    bb_pub_subscription_t sub = { .subtopics = list, .nsubtopics = 2 };
    TEST_ASSERT_TRUE(bb_pub_subscription_match(&sub, "power", NULL, 0));
}

// bb_pub_subscription_match: a NULL entry inside the tags array must be
// skipped safely rather than dereferenced.
void test_bb_pub_subscription_match_null_entry_in_tags_skipped(void)
{
    const char *tag_list[] = { NULL, "critical" };
    bb_pub_subscription_t sub = { .tags = tag_list, .ntags = 2 };
    const char *src_tags[] = { "critical" };
    TEST_ASSERT_TRUE(bb_pub_subscription_match(&sub, "power", src_tags, 1));
}

// bb_pub_subscription_match: a NULL entry inside the SOURCE tags array
// (tags[ti]) must be skipped safely -- distinct from a NULL entry in the
// subscription's own tags[si] array (already covered) and a NULL tags
// pointer entirely (already covered).
void test_bb_pub_subscription_match_null_entry_in_source_tags_skipped(void)
{
    const char *tag_list[] = { "critical" };
    bb_pub_subscription_t sub = { .tags = tag_list, .ntags = 1 };
    const char *src_tags[] = { NULL, "critical" };
    TEST_ASSERT_TRUE(bb_pub_subscription_match(&sub, "power", src_tags, 2));
}

// bb_pub_subscription_match: a NULL source-tags pointer with a nonzero
// ntags (a mismatched/defensive caller) must be skipped safely rather than
// dereferenced.
void test_bb_pub_subscription_match_null_tags_ptr_with_nonzero_ntags_skipped(void)
{
    const char *tag_list[] = { "critical" };
    bb_pub_subscription_t sub = { .tags = tag_list, .ntags = 1 };
    TEST_ASSERT_FALSE(bb_pub_subscription_match(&sub, "power", NULL, 1));
}

// ---------------------------------------------------------------------------
// tags reset on test_reset
// ---------------------------------------------------------------------------

void test_bb_pub_tags_cleared_after_test_reset(void)
{
    const char *tags_in[] = { "mining" };
    bb_pub_register_source_ex("power", sample_val, NULL, tags_in, 1);
    bb_pub_test_reset();
    // After reset, source count is 0 — register fresh without tags.
    bb_pub_register_source("power", sample_val, NULL);
    int ntags = -1;
    TEST_ASSERT_EQUAL(BB_OK, bb_pub_source_info_ex(0, NULL, NULL, NULL, NULL, NULL, NULL, &ntags));
    TEST_ASSERT_EQUAL(0, ntags);
}
