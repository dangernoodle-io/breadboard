#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "bb_core.h"
#include "bb_log.h"

/*
 * Config-driven log-level bootstrap (CONFIG_BB_LOG_DEFAULT_LEVEL /
 * CONFIG_BB_LOG_LEVELS). This file's code is portable — no ESP-IDF calls
 * here; splitting is delegated to bb_kv_parse (components/bb_kv), and the
 * actual level application goes through bb_log_level_set(), which is
 * already portable (no-op backend on host/Arduino, esp_log_level_set on
 * ESP-IDF). However the *feature* (auto-applying via bb_init's EARLY tier
 * from CONFIG_BB_LOG_DEFAULT_LEVEL/CONFIG_BB_LOG_LEVELS) is ESP-IDF only
 * today — it is not wired into the Arduino build_src_filter, so it is
 * absent on Arduino targets, not just a no-op.
 *
 * Private to bb_log — not part of the public API surface. Host tests reach
 * this header directly (same component, not "reaching into another
 * component"), mirroring bb_log_internal.h's existing test-visibility.
 */

/**
 * Map a level-name slice (not necessarily NUL-terminated) to bb_log_level_t.
 * Case-insensitive; one of: none, error, warn, info, debug, verbose.
 * Returns false for NULL name, zero length, an over-long slice (>=16 bytes,
 * no valid level name is that long), or an unrecognised name.
 * Pure — no side effects, no platform calls. Reuses bb_log_level_from_str
 * (bb_log.h) so the name<->level table has exactly one source of truth.
 */
bool bb_log_level_from_name(const char *name, size_t len, bb_log_level_t *out);

/**
 * bb_kv_cb_t-shaped callback (bb_kv.h): applies one already-split, already-
 * trimmed "tag"/"level" slice pair. An empty value, or an unrecognised
 * level name, is logged as a warning and skipped — never fatal. `ctx` is
 * unused (NULL). On success, calls bb_log_level_set(tag, level).
 *
 * bb_kv_parse itself skips entries with no '=' or an empty key before this
 * callback is ever invoked — those never reach here.
 */
void bb_log_config_apply_kv(const char *key, size_t key_len,
                            const char *val, size_t val_len, void *ctx);

/**
 * Apply a full "tag=level,tag2=level2,..." string via bb_kv_parse, using
 * bb_log_config_apply_kv as the per-pair callback. NULL or empty is a
 * no-op (bb_kv_parse's own contract).
 */
void bb_log_config_apply_levels(const char *s);

/**
 * Apply a default level string ("*") then a "tag=level,..." overrides
 * string. Pure aside from bb_log_level_set's side effect — no Kconfig/macro
 * dependency, so this is the host-testable seam for both branches of
 * bb_log_config_init (valid and invalid default_level_str) without needing
 * to recompile against different CONFIG_BB_LOG_DEFAULT_LEVEL values.
 * Always returns BB_OK — invalid config is logged and skipped, never fatal.
 */
bb_err_t bb_log_config_apply(const char *default_level_str, const char *levels_str);

/**
 * EARLY-tier bb_init entry point: thin wrapper calling bb_log_config_apply
 * with CONFIG_BB_LOG_DEFAULT_LEVEL / CONFIG_BB_LOG_LEVELS.
 */
bb_err_t bb_log_config_init(void);
