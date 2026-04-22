#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bb_nv.h"

#ifdef __cplusplus
extern "C" {
#endif

// Runtime info snapshot. Fields that the active backend cannot populate
// are zeroed / empty. Intended for GET /api/board.
typedef struct {
    char board[32];         // FIRMWARE_BOARD value, or empty
    char project_name[32];
    char version[32];
    char idf_version[32];
    char build_date[16];
    char build_time[16];
    char chip_model[16];
    uint8_t cores;
    char mac[18];           // lowercase colon-separated, or empty
    uint32_t flash_size;    // bytes, 0 if unknown
    uint32_t total_heap;
    uint32_t free_heap;
    uint32_t app_size;      // running partition size, 0 if no partitions
    char reset_reason[16];  // "power-on", "software", "panic", ...
    bool ota_validated;     // true unless running slot is PENDING_VERIFY
} bb_board_info_t;

// Populate out with a runtime snapshot. Returns BB_OK on success.
bb_err_t bb_board_get_info(bb_board_info_t *out);

// Discrete accessors. Numeric getters return 0 when the backend cannot
// determine the value; string getters fill out with an empty string
// and return BB_OK. All string getters expect out_size >= 1.
uint32_t bb_board_get_free_heap (void);
uint32_t bb_board_get_total_heap(void);
uint32_t bb_board_get_flash_size(void);
uint32_t bb_board_get_app_size  (void);
uint8_t  bb_board_get_cores     (void);

bb_err_t bb_board_get_chip_model   (char *out, size_t out_size);
bb_err_t bb_board_get_mac          (char *out, size_t out_size);
bb_err_t bb_board_get_idf_version  (char *out, size_t out_size);
bb_err_t bb_board_get_reset_reason (char *out, size_t out_size);

#ifdef ESP_PLATFORM

// Register GET /api/board returning bb_board_get_info() as JSON.
// server is bb_http_handle_t (declared as void* to avoid pulling
// http_server.h into bb_board consumers).
bb_err_t bb_board_register_routes(void *server);

#endif

#ifdef __cplusplus
}
#endif
