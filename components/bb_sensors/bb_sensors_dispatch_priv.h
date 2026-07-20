#pragma once

// Private: bb_sensors' bb_data-bind + bb_http_section-register step (bb_sensors
// PR-2, B1-828 epic). Portable (no ESP-IDF dependency) -- bb_data_bind()/
// bb_http_section_register_ns() are both portable calls; the ESP-IDF-only
// route registration itself is bb_http_section_init() (components/
// bb_http_server), called separately by bb_sensors_init()
// (platform/espidf/bb_sensors/bb_sensors.c). Split out so host tests can
// drive the bind+register step without an HTTP server.
#include "bb_core.h"

#ifdef __cplusplus
extern "C" {
#endif

// Binds the fan/power/thermal bb_data keys and registers the "/api/sensors/"
// bb_http_section namespace over them. Idempotent only in the sense
// bb_data_bind()/bb_http_section_register_ns() are (re-bind overrides,
// duplicate-prefix registration is rejected) -- call once per process
// lifetime (or once per bb_data_test_reset()+bb_http_section_test_reset()
// cycle in tests).
bb_err_t bb_sensors_bind_and_register(void);

#ifdef __cplusplus
}
#endif
