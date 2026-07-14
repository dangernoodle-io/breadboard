// test_serialize_fixture_meta -- cold-metadata table for the synthetic
// test_fixture_widget_t fixture (see test_serialize_fixture.h). Pure
// designated-initializer static data -- no logic, zero instrumented lines.

#include "bb_serialize_meta.h"

#include "test_serialize_fixture.h"

#include <stddef.h>

static const char *const s_region_enum[] = { "rg-a", "unset", NULL };

static const char *const s_serial_examples[] = { "\"widget-serial-001\"", NULL };
static const char *const s_epoch_examples[]  = { "1704067200", NULL };
static const char *const s_region_examples[] = { "\"rg-a\"", NULL };
static const char *const s_label_examples[]  = { "\"widget-01\"", NULL };

static const bb_serialize_field_meta_t s_widget_meta_rows[] = {
    {
        .key = "serial", .required = true, .min_len = 17,
        .title = "Serial number", .description = "Fixture widget serial number",
        .format = "serial", .examples = s_serial_examples,
    },
    {
        .key = "calibrated", .required = true,
        .title = "Calibrated", .description = "True once the fixture widget completed calibration",
    },
    {
        .key = "armed", .required = true,
        .title = "Armed", .description = "True once the fixture widget was armed",
    },
    {
        .key = "installed_epoch_s", .required = true,
        .has_min = true, .min = 0, .has_max = true, .max = 4102444800.0,  // year 2100
        .title = "Installed epoch", .description = "Unix time (seconds) the fixture widget was installed, or 0 if unknown",
        .examples = s_epoch_examples,
    },
    {
        .key = "region", .required = true, .min_len = 4,
        .enum_vals = s_region_enum,
        .title = "Region", .description = "Fixture widget install region code",
        .examples = s_region_examples,
    },
    {
        .key = "label", .required = false,
        .title = "Label", .description = "Fixture widget label, or null if not yet assigned",
        .examples = s_label_examples,
    },
    {
        .key = "tags", .required = true,
        .title = "Tags", .description = "Fixture widget tag list",
    },
};

const bb_serialize_desc_meta_t bb_fixture_widget_meta = {
    .type_name = "widget",
    .rows      = s_widget_meta_rows,
    .n_rows    = 7,
};
