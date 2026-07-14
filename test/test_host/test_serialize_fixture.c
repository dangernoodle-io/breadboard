// test_serialize_fixture -- synthetic bb_serialize_desc_t for the
// test_fixture_widget_t worked example (see test_serialize_fixture.h).

#include "test_serialize_fixture.h"

#include <stddef.h>

static const bb_serialize_field_t s_widget_fields[] = {
    { .key = "serial", .type = BB_TYPE_STR,
      .offset = offsetof(test_fixture_widget_t, serial), .max_len = 18 },
    { .key = "calibrated", .type = BB_TYPE_BOOL,
      .offset = offsetof(test_fixture_widget_t, calibrated) },
    { .key = "armed", .type = BB_TYPE_BOOL,
      .offset = offsetof(test_fixture_widget_t, armed) },
    { .key = "installed_epoch_s", .type = BB_TYPE_I64,
      .offset = offsetof(test_fixture_widget_t, installed_epoch_s) },
    { .key = "region", .type = BB_TYPE_STR,
      .offset = offsetof(test_fixture_widget_t, region), .max_len = 8 },
    { .key = "label", .type = BB_TYPE_STR_N,
      .offset = offsetof(test_fixture_widget_t, label) },
    { .key = "tags", .type = BB_TYPE_ARR,
      .offset = offsetof(test_fixture_widget_t, tags),
      .elem_type = BB_TYPE_STR, .max_len = 32 },
};

const bb_serialize_desc_t bb_fixture_widget_desc = {
    .type_name = "widget",
    .fields    = s_widget_fields,
    .n_fields  = 7,
    .snap_size = sizeof(test_fixture_widget_t),
};
