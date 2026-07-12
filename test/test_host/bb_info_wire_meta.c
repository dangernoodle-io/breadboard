// bb_info_wire_meta — the one worked-example cold-metadata table (B1-767
// PR-7), keyed by wire `key` against bb_info_wire_desc
// (components/bb_info/bb_info_wire_priv.h). Pure designated-initializer
// static data -- no logic, zero instrumented lines. Host-only (see
// bb_serialize_meta.h banner) -- never wired into a live route.

#include "bb_serialize_meta.h"

#include "../../components/bb_info/bb_info_wire_priv.h"

#include <stddef.h>

static const char *const s_time_source_enum[] = { "sntp", "none", NULL };

static const char *const s_mac_examples[] = { "\"aa:bb:cc:dd:ee:ff\"", NULL };
static const char *const s_boot_epoch_examples[] = { "1704067200", NULL };
static const char *const s_time_source_examples[] = { "\"sntp\"", NULL };
static const char *const s_hostname_examples[] = { "\"bb-test\"", NULL };

static const bb_serialize_field_meta_t s_info_wire_meta_rows[] = {
    {
        .key = "mac", .required = true, .min_len = 17,
        .title = "MAC address", .description = "Device station MAC, colon-separated hex",
        .format = "mac", .examples = s_mac_examples,
    },
    {
        .key = "ota_validated", .required = true,
        .title = "OTA validated", .description = "True once the running image passed its OTA validation check",
    },
    {
        .key = "time_valid", .required = true,
        .title = "Time valid", .description = "True once the device clock has been synchronized",
    },
    {
        .key = "boot_epoch_s", .required = true,
        .has_min = true, .min = 0, .has_max = true, .max = 4102444800.0,  // year 2100
        .title = "Boot epoch", .description = "Unix time (seconds) the device booted, or 0 if unknown",
        .examples = s_boot_epoch_examples,
    },
    {
        .key = "time_source", .required = true, .min_len = 4,
        .enum_vals = s_time_source_enum,
        .title = "Time source", .description = "Clock synchronization source",
        .examples = s_time_source_examples,
    },
    {
        .key = "hostname", .required = false,
        .title = "Hostname", .description = "mDNS hostname, or null if not yet assigned",
        .examples = s_hostname_examples,
    },
    {
        .key = "capabilities", .required = true,
        .title = "Capabilities", .description = "Composed-in optional-feature identifiers",
    },
};

const bb_serialize_desc_meta_t bb_info_wire_meta = {
    .type_name = "info",
    .rows      = s_info_wire_meta_rows,
    .n_rows    = 7,
};
