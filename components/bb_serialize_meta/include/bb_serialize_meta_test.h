#pragma once

#include <stdbool.h>

/**
 * @brief Host-only test hook: force every bb_serialize_meta_openapi_*
 * composer entry point (bb_serialize_meta_openapi_schema(),
 * bb_serialize_meta_openapi_fragment()) to fail with BB_ERR_NO_SPACE --
 * the same all-or-nothing overflow output a genuine undersized buffer
 * produces (out[0]='\0', *out_len=0) -- regardless of the caller's actual
 * buffer capacity.
 *
 * Exists so every compose-at-init fail-fast branch across the epic (e.g.
 * `if (ensure_*_patched() != BB_OK) { bb_log_e(...); return; }` in
 * bb_diag_storage_nvs.c, bb_mqtt_client_health_section.c, bb_temp.c) can be
 * genuinely exercised from a wiring test, ENGINE-side, without a per-
 * component seam or an artificially undersized static buffer at each call
 * site.
 *
 * Default state: force=false (normal compose). Only available when
 * BB_SERIALIZE_META_TESTING is defined.
 */
#ifdef BB_SERIALIZE_META_TESTING
void bb_serialize_meta_openapi_test_set_force_no_space(bool force);
#endif
