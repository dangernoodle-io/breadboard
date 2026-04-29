#include "bb_board.h"

#include <string.h>

bb_err_t bb_board_get_info(bb_board_info_t *out)
{
    if (!out) return BB_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));

    strncpy(out->board, "host", sizeof(out->board) - 1);
    strncpy(out->project_name, "host", sizeof(out->project_name) - 1);
    strncpy(out->version, "0.0.0-host", sizeof(out->version) - 1);
    strncpy(out->chip_model, "host", sizeof(out->chip_model) - 1);
    strncpy(out->idf_version, "0.0.0-host", sizeof(out->idf_version) - 1);
    out->cores = 1;

    return BB_OK;
}

uint32_t bb_board_get_free_heap(void)
{
    return 0;
}

uint32_t bb_board_get_total_heap(void)
{
    return 0;
}

uint32_t bb_board_get_flash_size(void)
{
    return 0;
}

uint32_t bb_board_get_app_size(void)
{
    return 0;
}

uint8_t bb_board_get_cores(void)
{
    return 1;
}

bb_err_t bb_board_get_chip_model(char *out, size_t out_size)
{
    if (!out || out_size == 0) return BB_ERR_INVALID_ARG;
    strncpy(out, "host", out_size - 1);
    out[out_size - 1] = '\0';
    return BB_OK;
}

bb_err_t bb_board_get_mac(char *out, size_t out_size)
{
    if (!out || out_size == 0) return BB_ERR_INVALID_ARG;
    out[0] = '\0';
    return BB_OK;
}

bb_err_t bb_board_get_idf_version(char *out, size_t out_size)
{
    if (!out || out_size == 0) return BB_ERR_INVALID_ARG;
    strncpy(out, "0.0.0-host", out_size - 1);
    out[out_size - 1] = '\0';
    return BB_OK;
}

bb_err_t bb_board_get_reset_reason(char *out, size_t out_size)
{
    if (!out || out_size == 0) return BB_ERR_INVALID_ARG;
    strncpy(out, "power-on", out_size - 1);
    out[out_size - 1] = '\0';
    return BB_OK;
}

size_t bb_board_heap_free_total(void)
{
    return 0;
}

size_t bb_board_heap_free_internal(void)
{
    return 0;
}

size_t bb_board_heap_minimum_ever(void)
{
    return 0;
}

uint32_t bb_board_chip_revision(void)
{
    return 0;
}

uint32_t bb_board_cpu_freq_mhz(void)
{
    return 0;
}
