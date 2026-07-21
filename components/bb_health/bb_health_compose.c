// bb_health_compose -- gather-then-stream composition for the
// /api/health document (B1-1100, PR-5 of 6, epic B1-1054). Portable: no
// ESP-IDF/FreeRTOS types, compiled for host + device (mirrors
// bb_health_emit.c/bb_health_wire.c's portability). The ESP-IDF route
// handler (platform/espidf/bb_health/bb_health.c) gathers the ROOT slice
// (ok/validated/network -- device-specific sources: bb_wifi/bb_mdns/
// bb_board) and hands it here; this file owns the section-registry walk
// and document streaming, neither of which needs a platform header --
// the "host-testable seam" the ESP-IDF handler stays a thin wrapper over
// (see test/test_host/test_bb_health_compose.c).
//
// GATHER-THEN-STREAM (the revert-detector's target behavior, see this
// file's own doc comment in bb_health_compose_priv.h): every registered
// section is filled into a request-scoped stack arena FIRST. If ANY fill
// returns non-BB_OK, this function sends a clean 500 (before any
// bb_http_resp_* call that would put normal-body bytes on the wire) and
// returns -- an interleaved gather/emit (reverting the phase split) would
// instead have already streamed the earlier sections' bytes before hitting
// the failing one, which the revert-detector test below is built to catch.
#include "bb_health_compose_priv.h"

#include "bb_health_section_priv.h"
#include "bb_serialize_compose.h"

bb_err_t bb_health_compose_and_stream(bb_http_request_t *req, const bb_health_wire_t *root)
{
    if (!req || !root) return BB_ERR_INVALID_ARG;

    uint16_t n = bb_health_section_count();
    if (n > BB_HEALTH_SECTION_TABLE_CAP) n = BB_HEALTH_SECTION_TABLE_CAP;  // LCOV_EXCL_BR_LINE -- bb_health_section_register() itself enforces this cap; bb_health_section_count() can never exceed it.

    // Phase 1 -- GATHER: fill every section into its own scratch slot.
    // Nothing has been written to `req` yet at this point.
    uint8_t arena[BB_HEALTH_SECTION_TABLE_CAP][BB_HEALTH_SECTION_SCRATCH_BYTES];
    bb_serialize_compose_entry_t section_entries[BB_HEALTH_SECTION_TABLE_CAP];

    for (uint16_t i = 0; i < n; i++) {
        const bb_health_section_t *sec = bb_health_section_get_by_index(i);
        if (!sec) {  // LCOV_EXCL_BR_LINE -- unreachable: idx < bb_health_section_count() always resolves via bb_health_section_get_by_index() (same registry, no concurrent mutation mid-request).
            // LCOV_EXCL_START
            return bb_http_send_json_error(req, 500, "{\"error\":\"health section registry inconsistent\"}");
            // LCOV_EXCL_STOP
        }

        bb_health_fill_args_t args = { .ctx = sec->ctx };
        bb_err_t fill_err = sec->fill(arena[i], &args);
        if (fill_err != BB_OK) {
            return bb_http_send_json_error(req, 500, "{\"error\":\"health section unavailable\"}");
        }

        section_entries[i] = (bb_serialize_compose_entry_t){
            .name = sec->name,
            .desc = sec->snap_desc,
            .snap = arena[i],
        };
    }

    // Phase 2 -- EMIT: stream the root slice (RAW, flat at the document
    // root) followed by every gathered section (OBJECT, named children).
    // `root` is read-only for the remainder of this call -- bb_serialize_
    // compose_entry_t.snap is `void *` only because gather-driven entries
    // need a writable slot; this entry has no gather fn, so it is never
    // written through.
    bb_serialize_compose_entry_t root_entry = { .desc = &bb_health_wire_desc, .snap = (void *)(uintptr_t)root };
    bb_serialize_compose_group_t groups[] = {
        { .entries = &root_entry,     .n = 1, .shape = BB_SERIALIZE_COMPOSE_RAW },
        { .entries = section_entries, .n = n, .shape = BB_SERIALIZE_COMPOSE_OBJECT },
    };

    // f64_shortest = true (MANDATORY, B1-1102): cJSON-print_number-identical
    // shortest float rendering -- required for on-device byte-identity with
    // today's /api/health (e.g. temp's soc_c must render "55.3", never
    // "55.300000").
    return bb_http_serialize_stream_compose_ex(req, groups, 2, true);
}
