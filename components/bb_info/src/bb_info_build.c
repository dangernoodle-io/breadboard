// bb_info_build — pure serializer + snapshot for the /api/info build subsection.
//
// This file compiles on both host and ESP-IDF.  No platform-specific headers
// are included here; bb_system_* and bb_board_* accessors are portable.
//
// The bb_cache "build" topic uses this serializer so the REST path
// (bb_cache_serialize_into) and the SSE path (bb_cache_post) are identical
// by construction.

#include "bb_info_build_priv.h"
#include "bb_board.h"
#include "bb_json.h"
#include "bb_system.h"
#include <string.h>

// ---------------------------------------------------------------------------
// Pure serializer (bb_cache_serialize_fn signature)
// ---------------------------------------------------------------------------

void bb_info_build_emit(bb_json_t obj, const void *snap)
{
    const bb_info_build_snap_t *s = (const bb_info_build_snap_t *)snap;
    bb_json_obj_set_string(obj, "version",      s->version);
    bb_json_obj_set_string(obj, "idf_version",  s->idf_version);
    bb_json_obj_set_string(obj, "build_date",   s->build_date);
    bb_json_obj_set_string(obj, "build_time",   s->build_time);
    bb_json_obj_set_string(obj, "project_name", s->project_name);
    bb_json_obj_set_string(obj, "chip_model",   s->chip_model);
    bb_json_obj_set_number(obj, "chip_revision",(double)s->chip_revision);
    bb_json_obj_set_number(obj, "cores",        (double)s->cores);
    bb_json_obj_set_number(obj, "cpu_freq_mhz", (double)s->cpu_freq_mhz);
    bb_json_obj_set_number(obj, "flash_size",   (double)s->flash_size);
    bb_json_obj_set_number(obj, "app_size",     (double)s->app_size);
    bb_json_obj_set_string(obj, "board",        s->board);
    bb_json_obj_set_string(obj, "app_sha256",   s->app_sha256);
}

// ---------------------------------------------------------------------------
// Snapshot capture
// ---------------------------------------------------------------------------

bb_err_t bb_info_build_capture(bb_info_build_snap_t *out)
{
    if (!out) return BB_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));

    // Version strings via bb_system — all accessors are contractually non-NULL
    strncpy(out->version,      bb_system_get_version(),      sizeof(out->version)      - 1);
    strncpy(out->idf_version,  bb_system_get_idf_version(),  sizeof(out->idf_version)  - 1);
    strncpy(out->build_date,   bb_system_get_build_date(),   sizeof(out->build_date)   - 1);
    strncpy(out->build_time,   bb_system_get_build_time(),   sizeof(out->build_time)   - 1);
    strncpy(out->project_name, bb_system_get_project_name(), sizeof(out->project_name) - 1);

    // Chip model via bb_board
    bb_board_get_chip_model(out->chip_model, sizeof(out->chip_model));

    out->chip_revision = bb_board_chip_revision();
    out->cores         = bb_board_get_cores();
    out->cpu_freq_mhz  = bb_board_cpu_freq_mhz();
    out->flash_size    = bb_board_get_flash_size();
    out->app_size      = bb_board_get_app_size();

    // Board name: use bb_board_info_t.board (FIRMWARE_BOARD value)
    bb_board_info_t binfo;
    bb_board_get_info(&binfo);
    strncpy(out->board, binfo.board, sizeof(out->board) - 1);

    // App SHA256 prefix
    bb_system_get_app_sha256(out->app_sha256, sizeof(out->app_sha256));

    return BB_OK;
}
