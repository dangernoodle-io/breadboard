#pragma once

// bb_http_core_steer_resolve — bb_http_server-internal (src-private, not in
// include/) — no consumer outside this component's own .c files.
//
// B1-1364 PR5: resolves the ESP-IDF httpd worker task's httpd_config_t.core_id
// so the worker never lands on a core another component has EXCLUSIVELY
// claimed (bb_wdt's core-claim mechanism, components/bb_wdt/include/bb_wdt.h)
// -- the second failure mode in KB 350 (httpd worker starvation; the first,
// idle-check polarity, is handled by the core-claim/reconfigure work already
// landed in #1192/#1197).
//
// Thin wrapper over bb_wdt_steer_core() (already pure + host-tested) that
// adds exactly one piece of NEW decision logic: mapping bb_wdt's portable
// BB_WDT_CORE_ANY sentinel (-1) to the caller's platform "no affinity" value
// (tskNO_AFFINITY on ESP-IDF, 0x7FFFFFFF -- NOT -1), since httpd_config_t.
// core_id is a FreeRTOS BaseType_t, not bb_wdt's int sentinel. Kept pure (no
// FreeRTOS/ESP-IDF types) by taking that sentinel as a parameter rather than
// hardcoding tskNO_AFFINITY here -- the espidf shell (platform/espidf/
// bb_http_server/bb_http.c) is the only real caller and passes
// (int32_t)tskNO_AFFINITY.
//
// - configured_core: the board's explicit configuration (BB_HTTP_TASK_CORE_ID,
//   bridged from CONFIG_BB_HTTP_TASK_CORE_ID, C default -1 == BB_WDT_CORE_ANY).
//   An explicit pin (0/1) always wins over steering -- see
//   bb_wdt_steer_core()'s own doc.
// - claimed_mask: bb_wdt's live claimed-core bitmask (bb_wdt_claimed_core_mask()),
//   read by the caller before this function runs (mirrors bb_task_resolve()'s
//   identical caller-reads-live-state convention).
// - num_cores: the real core count (configNUMBER_OF_CORES on the espidf
//   shell; any value in host tests) -- a unicore target (num_cores < 2) never
//   steers, matching bb_wdt_steer_core()'s own unicore short-circuit.
// - no_affinity: the caller's platform sentinel for "no core affinity",
//   returned whenever the steered result is BB_WDT_CORE_ANY.
//
// With an empty claimed_mask and configured_core == BB_WDT_CORE_ANY (the
// default, unconfigured case), this always returns `no_affinity` unchanged --
// identical to the pre-PR5 behavior of leaving httpd_config_t.core_id at
// HTTPD_DEFAULT_CONFIG()'s tskNO_AFFINITY.

#include <stdint.h>

int32_t bb_http_core_steer_resolve(int configured_core, uint32_t claimed_mask,
                                    int num_cores, int32_t no_affinity);
