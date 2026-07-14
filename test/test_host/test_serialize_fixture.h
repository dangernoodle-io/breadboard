#pragma once

// test_serialize_fixture -- a purely synthetic, host-only bb_serialize
// worked-example fixture. Exercises the same field-type diversity (string,
// bool, i64, str_n, arr_str) the generic bb_serialize_meta / bb_serialize_json
// golden test suites need (test_v2_golden.c, test_bb_serialize_meta_openapi.c,
// test_bb_serialize_meta_validate.c).
//
// This is NOT derived from, and has no relationship to, any production wire
// payload -- the "widget" shape and its field names/values are made up for
// test coverage only. See bb_health_wire_priv.h / bb_pub_info_wire_priv.h
// for the actual production descriptors these suites also cover.

#include "bb_serialize.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    char                   serial[18];
    bool                   calibrated;
    bool                   armed;
    int64_t                installed_epoch_s;
    char                   region[8];
    bb_serialize_str_n_t   label;
    bb_serialize_arr_str_t tags;
} test_fixture_widget_t;

extern const bb_serialize_desc_t bb_fixture_widget_desc;
