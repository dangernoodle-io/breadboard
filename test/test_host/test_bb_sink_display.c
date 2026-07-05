// Host tests for bb_sink_display's pure building blocks (caps->selector,
// bb_filter-backed selection, default formatter, value resolver, and the
// row table). Injection-first: no bb_cache/bb_cache_reactive/bb_timer/
// bb_display calls anywhere in the code under test here -- the espidf glue
// (platform/espidf/bb_sink_display/bb_sink_display.c) that wires those in is
// CI-smoke only, not host-covered (see the component's README).
#include "unity.h"
#include "bb_sink_display.h"

#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------

static bb_sink_display_field_t mk_field(uint8_t priority, bb_sink_display_kind_t kind,
                                        const char *cache_key, const char *json_path,
                                        const char *label, const char *unit,
                                        bb_sink_display_format_fn format)
{
    bb_sink_display_field_t f;
    memset(&f, 0, sizeof(f));
    f.attrs.priority       = priority;
    f.attrs.kind           = (uint16_t)kind;
    f.attrs.tag_mask       = 0;
    f.attrs.delivery_class = BB_ATTRS_DELIVERY_MUST;
    f.cache_key = cache_key;
    f.json_path = json_path;
    f.label     = label;
    f.unit      = unit;
    f.kind      = kind;
    f.format    = format;
    return f;
}

static bb_sink_display_caps_t mk_caps(uint8_t screen_tier, uint16_t max_fields,
                                      uint16_t supported_kinds)
{
    bb_sink_display_caps_t c;
    memset(&c, 0, sizeof(c));
    c.screen_tier      = screen_tier;
    c.max_fields       = max_fields;
    c.supports_lvgl    = false;
    c.supported_kinds  = supported_kinds;
    return c;
}

#define ALL_KINDS_MASK ((1u << BB_SINK_DISPLAY_KIND_INT) | (1u << BB_SINK_DISPLAY_KIND_FLOAT) | \
                        (1u << BB_SINK_DISPLAY_KIND_BOOL) | (1u << BB_SINK_DISPLAY_KIND_STRING))

// ---------------------------------------------------------------------------
// bb_sink_display_caps_to_selector
// ---------------------------------------------------------------------------

void test_bb_sink_display_caps_to_selector_maps_fields(void)
{
    bb_sink_display_caps_t caps = mk_caps(7, 3, ALL_KINDS_MASK);
    bb_filter_selector_t sel;
    memset(&sel, 0xAA, sizeof(sel));

    bb_sink_display_caps_to_selector(&caps, &sel);

    TEST_ASSERT_EQUAL_UINT16(3, sel.max_count);
    TEST_ASSERT_EQUAL_UINT8(7, sel.priority_max);
    TEST_ASSERT_EQUAL_UINT16(ALL_KINDS_MASK, sel.kind_mask);
    TEST_ASSERT_EQUAL_UINT32(0, sel.tag_mask);
    TEST_ASSERT_EQUAL_UINT8(0, sel.pressure);
    TEST_ASSERT_EQUAL_UINT8(BB_ATTRS_DELIVERY_MUST, sel.min_delivery);
}

void test_bb_sink_display_caps_to_selector_null_out_noop(void)
{
    bb_sink_display_caps_t caps = mk_caps(0, 0, 0);
    bb_sink_display_caps_to_selector(&caps, NULL); // must not crash
}

void test_bb_sink_display_caps_to_selector_null_caps_zeroes_out(void)
{
    bb_filter_selector_t sel;
    memset(&sel, 0xAA, sizeof(sel));

    bb_sink_display_caps_to_selector(NULL, &sel);

    TEST_ASSERT_EQUAL_UINT16(0, sel.max_count);
    TEST_ASSERT_EQUAL_UINT8(0, sel.priority_max);
    TEST_ASSERT_EQUAL_UINT16(0, sel.kind_mask);
}

// ---------------------------------------------------------------------------
// bb_sink_display_select
// ---------------------------------------------------------------------------

void test_bb_sink_display_select_orders_by_priority_and_truncates(void)
{
    bb_sink_display_field_t fields[3] = {
        mk_field(5, BB_SINK_DISPLAY_KIND_INT, "a", "x", "A", NULL, NULL),
        mk_field(1, BB_SINK_DISPLAY_KIND_INT, "b", "x", "B", NULL, NULL),
        mk_field(3, BB_SINK_DISPLAY_KIND_INT, "c", "x", "C", NULL, NULL),
    };
    bb_sink_display_caps_t caps = mk_caps(0xFF, 2, ALL_KINDS_MASK);
    const bb_sink_display_field_t *out[3] = {0};

    size_t n = bb_sink_display_select(fields, 3, &caps, out, 3);

    TEST_ASSERT_EQUAL_size_t(2, n); // max_fields=2 truncates
    TEST_ASSERT_EQUAL_PTR(&fields[1], out[0]); // priority 1 first
    TEST_ASSERT_EQUAL_PTR(&fields[2], out[1]); // priority 3 second
}

void test_bb_sink_display_select_kind_mask_gates(void)
{
    bb_sink_display_field_t fields[2] = {
        mk_field(0, BB_SINK_DISPLAY_KIND_INT, "a", "x", "A", NULL, NULL),
        mk_field(0, BB_SINK_DISPLAY_KIND_STRING, "b", "x", "B", NULL, NULL),
    };
    bb_sink_display_caps_t caps = mk_caps(0xFF, 8, (uint16_t)(1u << BB_SINK_DISPLAY_KIND_INT));
    const bb_sink_display_field_t *out[2] = {0};

    size_t n = bb_sink_display_select(fields, 2, &caps, out, 2);

    TEST_ASSERT_EQUAL_size_t(1, n);
    TEST_ASSERT_EQUAL_PTR(&fields[0], out[0]);
}

void test_bb_sink_display_select_null_args_return_zero(void)
{
    bb_sink_display_field_t fields[1] = { mk_field(0, BB_SINK_DISPLAY_KIND_INT, "a", "x", "A", NULL, NULL) };
    bb_sink_display_caps_t caps = mk_caps(0xFF, 8, ALL_KINDS_MASK);
    const bb_sink_display_field_t *out[1] = {0};

    TEST_ASSERT_EQUAL_size_t(0, bb_sink_display_select(NULL, 1, &caps, out, 1));
    TEST_ASSERT_EQUAL_size_t(0, bb_sink_display_select(fields, 1, NULL, out, 1));
    TEST_ASSERT_EQUAL_size_t(0, bb_sink_display_select(fields, 1, &caps, NULL, 1));
    TEST_ASSERT_EQUAL_size_t(0, bb_sink_display_select(fields, 1, &caps, out, 0));
}

void test_bb_sink_display_select_n_fields_over_capacity_truncated(void)
{
    bb_sink_display_field_t fields[BB_SINK_DISPLAY_MAX_FIELDS + 1];
    for (size_t i = 0; i < BB_SINK_DISPLAY_MAX_FIELDS + 1; i++) {
        fields[i] = mk_field((uint8_t)i, BB_SINK_DISPLAY_KIND_INT, "k", "x", "L", NULL, NULL);
    }
    bb_sink_display_caps_t caps = mk_caps(0xFF, BB_SINK_DISPLAY_MAX_FIELDS + 1, ALL_KINDS_MASK);
    const bb_sink_display_field_t *out[BB_SINK_DISPLAY_MAX_FIELDS + 1];

    size_t n = bb_sink_display_select(fields, BB_SINK_DISPLAY_MAX_FIELDS + 1, &caps, out,
                                      BB_SINK_DISPLAY_MAX_FIELDS + 1);

    // n_fields is clamped to BB_SINK_DISPLAY_MAX_FIELDS before filtering.
    TEST_ASSERT_EQUAL_size_t(BB_SINK_DISPLAY_MAX_FIELDS, n);
}

// ---------------------------------------------------------------------------
// bb_sink_display_format_default
// ---------------------------------------------------------------------------

void test_bb_sink_display_format_default_int(void)
{
    bb_sink_display_field_t f = mk_field(0, BB_SINK_DISPLAY_KIND_INT, "k", "v", "Vout", "mV", NULL);
    bb_json_t doc = bb_json_parse("{\"v\":1200}", 10);
    bb_json_t item = bb_json_obj_get_item(doc, "v");
    char out[64];

    bb_sink_display_format_default(&f, item, out, sizeof(out));

    TEST_ASSERT_EQUAL_STRING("Vout: 1200 mV", out);
    bb_json_free(doc);
}

void test_bb_sink_display_format_default_float(void)
{
    bb_sink_display_field_t f = mk_field(0, BB_SINK_DISPLAY_KIND_FLOAT, "k", "v", "Temp", "C", NULL);
    bb_json_t doc = bb_json_parse("{\"v\":42.5}", 10);
    bb_json_t item = bb_json_obj_get_item(doc, "v");
    char out[64];

    bb_sink_display_format_default(&f, item, out, sizeof(out));

    TEST_ASSERT_EQUAL_STRING("Temp: 42.50 C", out);
    bb_json_free(doc);
}

void test_bb_sink_display_format_default_bool_true(void)
{
    bb_sink_display_field_t f = mk_field(0, BB_SINK_DISPLAY_KIND_BOOL, "k", "v", "Link", NULL, NULL);
    bb_json_t doc = bb_json_parse("{\"v\":true}", 10);
    bb_json_t item = bb_json_obj_get_item(doc, "v");
    char out[64];

    bb_sink_display_format_default(&f, item, out, sizeof(out));

    TEST_ASSERT_EQUAL_STRING("Link: true", out); // no unit -> no trailing segment
    bb_json_free(doc);
}

void test_bb_sink_display_format_default_bool_false(void)
{
    bb_sink_display_field_t f = mk_field(0, BB_SINK_DISPLAY_KIND_BOOL, "k", "v", "Link", "", NULL);
    bb_json_t doc = bb_json_parse("{\"v\":false}", 11);
    bb_json_t item = bb_json_obj_get_item(doc, "v");
    char out[64];

    bb_sink_display_format_default(&f, item, out, sizeof(out));

    TEST_ASSERT_EQUAL_STRING("Link: false", out);
    bb_json_free(doc);
}

void test_bb_sink_display_format_default_string(void)
{
    bb_sink_display_field_t f = mk_field(0, BB_SINK_DISPLAY_KIND_STRING, "k", "v", "SSID", NULL, NULL);
    bb_json_t doc = bb_json_parse("{\"v\":\"home\"}", 13);
    bb_json_t item = bb_json_obj_get_item(doc, "v");
    char out[64];

    bb_sink_display_format_default(&f, item, out, sizeof(out));

    TEST_ASSERT_EQUAL_STRING("SSID: home", out);
    bb_json_free(doc);
}

void test_bb_sink_display_format_default_null_item_renders_dashes(void)
{
    bb_sink_display_field_t f = mk_field(0, BB_SINK_DISPLAY_KIND_INT, "k", "v", "X", "u", NULL);
    char out[64];

    bb_sink_display_format_default(&f, NULL, out, sizeof(out));

    TEST_ASSERT_EQUAL_STRING("X: --", out);
}

void test_bb_sink_display_format_default_mismatched_kind_renders_dashes(void)
{
    // Declared kind INT but the item is a string -- not a number.
    bb_sink_display_field_t f = mk_field(0, BB_SINK_DISPLAY_KIND_INT, "k", "v", "X", NULL, NULL);
    bb_json_t doc = bb_json_parse("{\"v\":\"nope\"}", 13);
    bb_json_t item = bb_json_obj_get_item(doc, "v");
    char out[64];

    bb_sink_display_format_default(&f, item, out, sizeof(out));

    TEST_ASSERT_EQUAL_STRING("X: --", out);
    bb_json_free(doc);
}

void test_bb_sink_display_format_default_float_mismatched_kind_renders_dashes(void)
{
    bb_sink_display_field_t f = mk_field(0, BB_SINK_DISPLAY_KIND_FLOAT, "k", "v", "X", NULL, NULL);
    bb_json_t doc = bb_json_parse("{\"v\":\"nope\"}", 13);
    bb_json_t item = bb_json_obj_get_item(doc, "v");
    char out[64];

    bb_sink_display_format_default(&f, item, out, sizeof(out));

    TEST_ASSERT_EQUAL_STRING("X: --", out);
    bb_json_free(doc);
}

void test_bb_sink_display_format_default_float_null_item_renders_dashes(void)
{
    bb_sink_display_field_t f = mk_field(0, BB_SINK_DISPLAY_KIND_FLOAT, "k", "v", "X", NULL, NULL);
    char out[64];

    bb_sink_display_format_default(&f, NULL, out, sizeof(out));

    TEST_ASSERT_EQUAL_STRING("X: --", out);
}

void test_bb_sink_display_format_default_bool_null_item_renders_dashes(void)
{
    bb_sink_display_field_t f = mk_field(0, BB_SINK_DISPLAY_KIND_BOOL, "k", "v", "X", NULL, NULL);
    char out[64];

    bb_sink_display_format_default(&f, NULL, out, sizeof(out));

    TEST_ASSERT_EQUAL_STRING("X: --", out);
}

void test_bb_sink_display_format_default_string_kind_null_item_renders_dashes(void)
{
    bb_sink_display_field_t f = mk_field(0, BB_SINK_DISPLAY_KIND_STRING, "k", "v", "X", NULL, NULL);
    char out[64];

    bb_sink_display_format_default(&f, NULL, out, sizeof(out));

    TEST_ASSERT_EQUAL_STRING("X: --", out);
}

void test_bb_sink_display_format_default_string_kind_non_string_item_renders_dashes(void)
{
    bb_sink_display_field_t f = mk_field(0, BB_SINK_DISPLAY_KIND_STRING, "k", "v", "X", NULL, NULL);
    bb_json_t doc = bb_json_parse("{\"v\":123}", 9);
    bb_json_t item = bb_json_obj_get_item(doc, "v");
    char out[64];

    bb_sink_display_format_default(&f, item, out, sizeof(out));

    TEST_ASSERT_EQUAL_STRING("X: --", out);
    bb_json_free(doc);
}

void test_bb_sink_display_format_default_unknown_kind_renders_dashes(void)
{
    bb_sink_display_field_t f = mk_field(0, BB_SINK_DISPLAY_KIND_INT, "k", "v", "X", NULL, NULL);
    f.kind = (bb_sink_display_kind_t)99; // defensive residual: unreachable via normal construction
    bb_json_t doc = bb_json_parse("{\"v\":1}", 7);
    bb_json_t item = bb_json_obj_get_item(doc, "v");
    char out[64];

    bb_sink_display_format_default(&f, item, out, sizeof(out));

    TEST_ASSERT_EQUAL_STRING("X: --", out);
    bb_json_free(doc);
}

void test_bb_sink_display_format_default_null_label_and_unit(void)
{
    bb_sink_display_field_t f = mk_field(0, BB_SINK_DISPLAY_KIND_INT, "k", "v", NULL, NULL, NULL);
    bb_json_t doc = bb_json_parse("{\"v\":5}", 7);
    bb_json_t item = bb_json_obj_get_item(doc, "v");
    char out[64];

    bb_sink_display_format_default(&f, item, out, sizeof(out));

    TEST_ASSERT_EQUAL_STRING(": 5", out);
    bb_json_free(doc);
}

void test_bb_sink_display_format_default_null_args_noop(void)
{
    bb_sink_display_field_t f = mk_field(0, BB_SINK_DISPLAY_KIND_INT, "k", "v", "X", NULL, NULL);
    char out[16] = "sentinel";

    bb_sink_display_format_default(NULL, NULL, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("sentinel", out); // untouched: field NULL

    bb_sink_display_format_default(&f, NULL, NULL, 8); // out NULL
    bb_sink_display_format_default(&f, NULL, out, 0);  // out_cap 0
}

// Custom format_fn: exercises field->format override path.
static void custom_format_upper(const bb_sink_display_field_t *field, bb_json_t item,
                                char *out, size_t out_cap)
{
    (void)field;
    (void)item;
    snprintf(out, out_cap, "CUSTOM");
}

// ---------------------------------------------------------------------------
// bb_sink_display_resolve_field
// ---------------------------------------------------------------------------

void test_bb_sink_display_resolve_field_success(void)
{
    bb_sink_display_field_t f = mk_field(0, BB_SINK_DISPLAY_KIND_INT, "k", "v", "X", "u", NULL);
    const char *data = "{\"v\":7}";
    char out[64];

    bool ok = bb_sink_display_resolve_field(&f, data, strlen(data), out, sizeof(out));

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_STRING("X: 7 u", out);
}

void test_bb_sink_display_resolve_field_uses_custom_format(void)
{
    bb_sink_display_field_t f = mk_field(0, BB_SINK_DISPLAY_KIND_INT, "k", "v", "X", "u",
                                         custom_format_upper);
    const char *data = "{\"v\":7}";
    char out[64];

    bool ok = bb_sink_display_resolve_field(&f, data, strlen(data), out, sizeof(out));

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_STRING("CUSTOM", out);
}

void test_bb_sink_display_resolve_field_missing_path_returns_false(void)
{
    bb_sink_display_field_t f = mk_field(0, BB_SINK_DISPLAY_KIND_INT, "k", "missing", "X", NULL, NULL);
    const char *data = "{\"v\":7}";
    char out[64];

    TEST_ASSERT_FALSE(bb_sink_display_resolve_field(&f, data, strlen(data), out, sizeof(out)));
}

void test_bb_sink_display_resolve_field_invalid_json_returns_false(void)
{
    bb_sink_display_field_t f = mk_field(0, BB_SINK_DISPLAY_KIND_INT, "k", "v", "X", NULL, NULL);
    const char *data = "{not json";
    char out[64];

    TEST_ASSERT_FALSE(bb_sink_display_resolve_field(&f, data, strlen(data), out, sizeof(out)));
}

void test_bb_sink_display_resolve_field_null_args_return_false(void)
{
    bb_sink_display_field_t f = mk_field(0, BB_SINK_DISPLAY_KIND_INT, "k", "v", "X", NULL, NULL);
    char out[64];

    TEST_ASSERT_FALSE(bb_sink_display_resolve_field(NULL, "{}", 2, out, sizeof(out)));
    TEST_ASSERT_FALSE(bb_sink_display_resolve_field(&f, NULL, 2, out, sizeof(out)));
    TEST_ASSERT_FALSE(bb_sink_display_resolve_field(&f, "{}", 0, out, sizeof(out)));
    TEST_ASSERT_FALSE(bb_sink_display_resolve_field(&f, "{}", 2, NULL, sizeof(out)));
    TEST_ASSERT_FALSE(bb_sink_display_resolve_field(&f, "{}", 2, out, 0));
}

// ---------------------------------------------------------------------------
// bb_sink_display_table_*
// ---------------------------------------------------------------------------

void test_bb_sink_display_table_init_zeroes(void)
{
    bb_sink_display_table_t t;
    memset(&t, 0xAA, sizeof(t));

    bb_sink_display_table_init(&t);

    TEST_ASSERT_EQUAL_size_t(0, t.n_entries);
}

void test_bb_sink_display_table_init_null_noop(void)
{
    bb_sink_display_table_init(NULL); // must not crash
}

void test_bb_sink_display_table_add_is_idempotent(void)
{
    bb_sink_display_table_t t;
    bb_sink_display_table_init(&t);
    bb_sink_display_field_t f = mk_field(0, BB_SINK_DISPLAY_KIND_INT, "k", "v", "X", NULL, NULL);

    TEST_ASSERT_EQUAL_INT(BB_OK, bb_sink_display_table_add(&t, &f));
    TEST_ASSERT_EQUAL_size_t(1, t.n_entries);
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_sink_display_table_add(&t, &f)); // idempotent
    TEST_ASSERT_EQUAL_size_t(1, t.n_entries);
}

void test_bb_sink_display_table_add_null_args_invalid(void)
{
    bb_sink_display_table_t t;
    bb_sink_display_table_init(&t);
    bb_sink_display_field_t f = mk_field(0, BB_SINK_DISPLAY_KIND_INT, "k", "v", "X", NULL, NULL);

    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, bb_sink_display_table_add(NULL, &f));
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, bb_sink_display_table_add(&t, NULL));
}

void test_bb_sink_display_table_add_full_returns_no_space(void)
{
    bb_sink_display_table_t t;
    bb_sink_display_table_init(&t);
    bb_sink_display_field_t fields[BB_SINK_DISPLAY_MAX_FIELDS + 1];
    for (size_t i = 0; i < BB_SINK_DISPLAY_MAX_FIELDS + 1; i++) {
        fields[i] = mk_field((uint8_t)i, BB_SINK_DISPLAY_KIND_INT, "k", "v", "X", NULL, NULL);
    }
    for (size_t i = 0; i < BB_SINK_DISPLAY_MAX_FIELDS; i++) {
        TEST_ASSERT_EQUAL_INT(BB_OK, bb_sink_display_table_add(&t, &fields[i]));
    }

    TEST_ASSERT_EQUAL_INT(BB_ERR_NO_SPACE, bb_sink_display_table_add(&t, &fields[BB_SINK_DISPLAY_MAX_FIELDS]));
}

void test_bb_sink_display_table_remove_by_key_removes_matching(void)
{
    bb_sink_display_table_t t;
    bb_sink_display_table_init(&t);
    bb_sink_display_field_t f0 = mk_field(0, BB_SINK_DISPLAY_KIND_INT, "shared", "a", "A", NULL, NULL);
    bb_sink_display_field_t f1 = mk_field(0, BB_SINK_DISPLAY_KIND_INT, "shared", "b", "B", NULL, NULL);
    bb_sink_display_field_t f2 = mk_field(0, BB_SINK_DISPLAY_KIND_INT, "other", "c", "C", NULL, NULL);
    bb_sink_display_table_add(&t, &f0);
    bb_sink_display_table_add(&t, &f1);
    bb_sink_display_table_add(&t, &f2);

    size_t removed = bb_sink_display_table_remove_by_key(&t, "shared");

    TEST_ASSERT_EQUAL_size_t(2, removed);
    TEST_ASSERT_EQUAL_size_t(1, t.n_entries);
    TEST_ASSERT_EQUAL_PTR(&f2, t.entries[0].row.field);
}

void test_bb_sink_display_table_remove_by_key_no_match_returns_zero(void)
{
    bb_sink_display_table_t t;
    bb_sink_display_table_init(&t);
    bb_sink_display_field_t f0 = mk_field(0, BB_SINK_DISPLAY_KIND_INT, "a", "x", "A", NULL, NULL);
    bb_sink_display_table_add(&t, &f0);

    TEST_ASSERT_EQUAL_size_t(0, bb_sink_display_table_remove_by_key(&t, "nope"));
}

void test_bb_sink_display_table_remove_by_key_field_without_cache_key_skipped(void)
{
    bb_sink_display_table_t t;
    bb_sink_display_table_init(&t);
    bb_sink_display_field_t f = mk_field(0, BB_SINK_DISPLAY_KIND_INT, NULL, "x", "X", NULL, NULL);
    bb_sink_display_table_add(&t, &f);

    TEST_ASSERT_EQUAL_size_t(0, bb_sink_display_table_remove_by_key(&t, "anything"));
    TEST_ASSERT_EQUAL_size_t(1, t.n_entries);
}

void test_bb_sink_display_table_remove_by_key_null_args_returns_zero(void)
{
    bb_sink_display_table_t t;
    bb_sink_display_table_init(&t);

    TEST_ASSERT_EQUAL_size_t(0, bb_sink_display_table_remove_by_key(NULL, "a"));
    TEST_ASSERT_EQUAL_size_t(0, bb_sink_display_table_remove_by_key(&t, NULL));
}

void test_bb_sink_display_table_apply_change_updates_row(void)
{
    bb_sink_display_table_t t;
    bb_sink_display_table_init(&t);
    bb_sink_display_field_t f = mk_field(0, BB_SINK_DISPLAY_KIND_INT, "k", "v", "X", NULL, NULL);
    bb_sink_display_table_add(&t, &f);

    bb_err_t err = bb_sink_display_table_apply_change(&t, &f, "X: 5", 1000);

    TEST_ASSERT_EQUAL_INT(BB_OK, err);
    TEST_ASSERT_EQUAL_STRING("X: 5", t.entries[0].row.value);
    TEST_ASSERT_EQUAL_UINT64(1000, t.entries[0].last_seen_ms);
    TEST_ASSERT_FALSE(t.entries[0].row.stale);
    TEST_ASSERT_TRUE(t.entries[0].dirty);
}

void test_bb_sink_display_table_apply_change_clears_stale(void)
{
    bb_sink_display_table_t t;
    bb_sink_display_table_init(&t);
    bb_sink_display_field_t f = mk_field(0, BB_SINK_DISPLAY_KIND_INT, "k", "v", "X", NULL, NULL);
    bb_sink_display_table_add(&t, &f);
    t.entries[0].row.stale = true;

    bb_sink_display_table_apply_change(&t, &f, "fresh", 42);

    TEST_ASSERT_FALSE(t.entries[0].row.stale);
}

void test_bb_sink_display_table_apply_change_not_found(void)
{
    bb_sink_display_table_t t;
    bb_sink_display_table_init(&t);
    bb_sink_display_field_t f = mk_field(0, BB_SINK_DISPLAY_KIND_INT, "k", "v", "X", NULL, NULL);

    TEST_ASSERT_EQUAL_INT(BB_ERR_NOT_FOUND, bb_sink_display_table_apply_change(&t, &f, "v", 1));
}

void test_bb_sink_display_table_apply_change_null_args_invalid(void)
{
    bb_sink_display_table_t t;
    bb_sink_display_table_init(&t);
    bb_sink_display_field_t f = mk_field(0, BB_SINK_DISPLAY_KIND_INT, "k", "v", "X", NULL, NULL);

    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, bb_sink_display_table_apply_change(NULL, &f, "v", 1));
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, bb_sink_display_table_apply_change(&t, NULL, "v", 1));
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, bb_sink_display_table_apply_change(&t, &f, NULL, 1));
}

void test_bb_sink_display_table_sweep_fresh_stays_fresh(void)
{
    bb_sink_display_table_t t;
    bb_sink_display_table_init(&t);
    bb_sink_display_field_t f = mk_field(0, BB_SINK_DISPLAY_KIND_INT, "k", "v", "X", NULL, NULL);
    bb_sink_display_table_add(&t, &f);
    bb_sink_display_table_apply_change(&t, &f, "v", 1000);

    size_t dropped = bb_sink_display_table_sweep(&t, 1500, 5000, 10000);

    TEST_ASSERT_EQUAL_size_t(0, dropped);
    TEST_ASSERT_EQUAL_size_t(1, t.n_entries);
    TEST_ASSERT_FALSE(t.entries[0].row.stale);
}

void test_bb_sink_display_table_sweep_marks_stale_transition_once(void)
{
    bb_sink_display_table_t t;
    bb_sink_display_table_init(&t);
    bb_sink_display_field_t f = mk_field(0, BB_SINK_DISPLAY_KIND_INT, "k", "v", "X", NULL, NULL);
    bb_sink_display_table_add(&t, &f);
    bb_sink_display_table_apply_change(&t, &f, "v", 1000);

    // First sweep crosses the stale threshold: transition -> dirty set.
    bb_sink_display_table_sweep(&t, 7000, 5000, 20000);
    TEST_ASSERT_TRUE(t.entries[0].row.stale);
    TEST_ASSERT_TRUE(t.entries[0].dirty);

    // Clear dirty (as collect_dirty would), then sweep again while still
    // stale -- no repeat churn (already-stale branch).
    t.entries[0].dirty = false;
    bb_sink_display_table_sweep(&t, 8000, 5000, 20000);
    TEST_ASSERT_FALSE(t.entries[0].dirty);
}

void test_bb_sink_display_table_sweep_evicts_past_evict_after(void)
{
    bb_sink_display_table_t t;
    bb_sink_display_table_init(&t);
    bb_sink_display_field_t f = mk_field(0, BB_SINK_DISPLAY_KIND_INT, "k", "v", "X", NULL, NULL);
    bb_sink_display_table_add(&t, &f);
    bb_sink_display_table_apply_change(&t, &f, "v", 1000);

    size_t dropped = bb_sink_display_table_sweep(&t, 1000 + 20000, 5000, 20000);

    TEST_ASSERT_EQUAL_size_t(1, dropped);
    TEST_ASSERT_EQUAL_size_t(0, t.n_entries);
}

void test_bb_sink_display_table_sweep_now_before_last_seen_treats_age_zero(void)
{
    bb_sink_display_table_t t;
    bb_sink_display_table_init(&t);
    bb_sink_display_field_t f = mk_field(0, BB_SINK_DISPLAY_KIND_INT, "k", "v", "X", NULL, NULL);
    bb_sink_display_table_add(&t, &f);
    bb_sink_display_table_apply_change(&t, &f, "v", 5000);

    size_t dropped = bb_sink_display_table_sweep(&t, 100 /* < last_seen_ms */, 100, 200);

    TEST_ASSERT_EQUAL_size_t(0, dropped);
    TEST_ASSERT_FALSE(t.entries[0].row.stale);
}

void test_bb_sink_display_table_sweep_compacts_survivors_after_earlier_evict(void)
{
    bb_sink_display_table_t t;
    bb_sink_display_table_init(&t);
    bb_sink_display_field_t f0 = mk_field(0, BB_SINK_DISPLAY_KIND_INT, "a", "v", "A", NULL, NULL);
    bb_sink_display_field_t f1 = mk_field(0, BB_SINK_DISPLAY_KIND_INT, "b", "v", "B", NULL, NULL);
    bb_sink_display_table_add(&t, &f0);
    bb_sink_display_table_add(&t, &f1);
    bb_sink_display_table_apply_change(&t, &f0, "old", 0);      // will be evicted
    bb_sink_display_table_apply_change(&t, &f1, "fresh", 9000); // survives, shifted into slot 0

    size_t dropped = bb_sink_display_table_sweep(&t, 9000, 5000, 1000);

    TEST_ASSERT_EQUAL_size_t(1, dropped);
    TEST_ASSERT_EQUAL_size_t(1, t.n_entries);
    TEST_ASSERT_EQUAL_PTR(&f1, t.entries[0].row.field);
}

void test_bb_sink_display_table_sweep_null_table_returns_zero(void)
{
    TEST_ASSERT_EQUAL_size_t(0, bb_sink_display_table_sweep(NULL, 0, 0, 0));
}

void test_bb_sink_display_table_collect_dirty_clears_flags(void)
{
    bb_sink_display_table_t t;
    bb_sink_display_table_init(&t);
    bb_sink_display_field_t f0 = mk_field(0, BB_SINK_DISPLAY_KIND_INT, "a", "v", "A", NULL, NULL);
    bb_sink_display_field_t f1 = mk_field(0, BB_SINK_DISPLAY_KIND_INT, "b", "v", "B", NULL, NULL);
    bb_sink_display_table_add(&t, &f0);
    bb_sink_display_table_add(&t, &f1);
    bb_sink_display_table_apply_change(&t, &f0, "va", 1);
    // f1 left non-dirty (never applied a change).

    const bb_sink_display_row_t *out[4] = {0};
    size_t n = bb_sink_display_table_collect_dirty(&t, out, 4);

    TEST_ASSERT_EQUAL_size_t(1, n);
    TEST_ASSERT_EQUAL_PTR(&f0, out[0]->field);
    TEST_ASSERT_FALSE(t.entries[0].dirty);

    // Second collect after clearing: nothing left dirty.
    n = bb_sink_display_table_collect_dirty(&t, out, 4);
    TEST_ASSERT_EQUAL_size_t(0, n);
}

void test_bb_sink_display_table_collect_dirty_respects_out_cap(void)
{
    bb_sink_display_table_t t;
    bb_sink_display_table_init(&t);
    bb_sink_display_field_t f0 = mk_field(0, BB_SINK_DISPLAY_KIND_INT, "a", "v", "A", NULL, NULL);
    bb_sink_display_field_t f1 = mk_field(0, BB_SINK_DISPLAY_KIND_INT, "b", "v", "B", NULL, NULL);
    bb_sink_display_table_add(&t, &f0);
    bb_sink_display_table_add(&t, &f1);
    bb_sink_display_table_apply_change(&t, &f0, "va", 1);
    bb_sink_display_table_apply_change(&t, &f1, "vb", 1);

    const bb_sink_display_row_t *out[1] = {0};
    size_t n = bb_sink_display_table_collect_dirty(&t, out, 1);

    TEST_ASSERT_EQUAL_size_t(1, n);
    TEST_ASSERT_TRUE(t.entries[1].dirty); // second entry left dirty for next tick
}

void test_bb_sink_display_table_collect_dirty_null_args_returns_zero(void)
{
    bb_sink_display_table_t t;
    bb_sink_display_table_init(&t);
    const bb_sink_display_row_t *out[1];

    TEST_ASSERT_EQUAL_size_t(0, bb_sink_display_table_collect_dirty(NULL, out, 1));
    TEST_ASSERT_EQUAL_size_t(0, bb_sink_display_table_collect_dirty(&t, NULL, 1));
    TEST_ASSERT_EQUAL_size_t(0, bb_sink_display_table_collect_dirty(&t, out, 0));
}

// ---------------------------------------------------------------------------
// bb_sink_display_validate_config
// ---------------------------------------------------------------------------

static void noop_custom(const bb_sink_display_row_t *rows, size_t n, void *ctx)
{
    (void)rows; (void)n; (void)ctx;
}

static bb_sink_display_config_t mk_cfg(bb_sink_display_policy_t kind,
                                        bb_sink_display_render_fn custom,
                                        uint32_t stale_after_ms, uint32_t evict_after_ms,
                                        const void *display)
{
    bb_sink_display_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.kind           = kind;
    cfg.custom         = custom;
    cfg.rate_limit_ms  = 1000;
    cfg.stale_after_ms = stale_after_ms;
    cfg.evict_after_ms = evict_after_ms;
    cfg.display        = display;
    return cfg;
}

void test_bb_sink_display_validate_config_accepts_valid_default_lines(void)
{
    bb_sink_display_caps_t caps = mk_caps(0xFF, 8, ALL_KINDS_MASK);
    bb_sink_display_config_t cfg = mk_cfg(BB_SINK_DISPLAY_POLICY_DEFAULT_LINES, NULL, 5000, 20000, NULL);

    TEST_ASSERT_EQUAL_INT(BB_OK, bb_sink_display_validate_config(&caps, &cfg));
}

void test_bb_sink_display_validate_config_accepts_valid_custom(void)
{
    bb_sink_display_caps_t caps = mk_caps(0xFF, 8, ALL_KINDS_MASK);
    bb_sink_display_config_t cfg = mk_cfg(BB_SINK_DISPLAY_POLICY_CUSTOM, noop_custom, 5000, 20000, NULL);

    TEST_ASSERT_EQUAL_INT(BB_OK, bb_sink_display_validate_config(&caps, &cfg));
}

void test_bb_sink_display_validate_config_null_caps_invalid(void)
{
    bb_sink_display_config_t cfg = mk_cfg(BB_SINK_DISPLAY_POLICY_DEFAULT_LINES, NULL, 5000, 20000, NULL);

    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, bb_sink_display_validate_config(NULL, &cfg));
}

void test_bb_sink_display_validate_config_null_cfg_invalid(void)
{
    bb_sink_display_caps_t caps = mk_caps(0xFF, 8, ALL_KINDS_MASK);

    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, bb_sink_display_validate_config(&caps, NULL));
}

void test_bb_sink_display_validate_config_custom_without_fn_invalid(void)
{
    bb_sink_display_caps_t caps = mk_caps(0xFF, 8, ALL_KINDS_MASK);
    bb_sink_display_config_t cfg = mk_cfg(BB_SINK_DISPLAY_POLICY_CUSTOM, NULL, 5000, 20000, NULL);

    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, bb_sink_display_validate_config(&caps, &cfg));
}

void test_bb_sink_display_validate_config_display_seam_unsupported(void)
{
    bb_sink_display_caps_t caps = mk_caps(0xFF, 8, ALL_KINDS_MASK);
    int sentinel;
    bb_sink_display_config_t cfg = mk_cfg(BB_SINK_DISPLAY_POLICY_DEFAULT_LINES, NULL, 5000, 20000, &sentinel);

    TEST_ASSERT_EQUAL_INT(BB_ERR_UNSUPPORTED, bb_sink_display_validate_config(&caps, &cfg));
}

void test_bb_sink_display_validate_config_evict_equal_stale_invalid(void)
{
    bb_sink_display_caps_t caps = mk_caps(0xFF, 8, ALL_KINDS_MASK);
    bb_sink_display_config_t cfg = mk_cfg(BB_SINK_DISPLAY_POLICY_DEFAULT_LINES, NULL, 5000, 5000, NULL);

    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, bb_sink_display_validate_config(&caps, &cfg));
}

void test_bb_sink_display_validate_config_evict_less_than_stale_invalid(void)
{
    bb_sink_display_caps_t caps = mk_caps(0xFF, 8, ALL_KINDS_MASK);
    bb_sink_display_config_t cfg = mk_cfg(BB_SINK_DISPLAY_POLICY_DEFAULT_LINES, NULL, 5000, 1000, NULL);

    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, bb_sink_display_validate_config(&caps, &cfg));
}

void test_bb_sink_display_validate_config_evict_greater_than_stale_valid(void)
{
    bb_sink_display_caps_t caps = mk_caps(0xFF, 8, ALL_KINDS_MASK);
    bb_sink_display_config_t cfg = mk_cfg(BB_SINK_DISPLAY_POLICY_DEFAULT_LINES, NULL, 5000, 5001, NULL);

    TEST_ASSERT_EQUAL_INT(BB_OK, bb_sink_display_validate_config(&caps, &cfg));
}
