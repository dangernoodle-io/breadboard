#include "bb_diag.h"
#include "bb_core.h"
#include <string.h>

// Host stubs for panic capture (always disabled on host)

bool bb_diag_panic_available(void)
{
    return false;
}

bb_err_t bb_diag_panic_get(char *out, size_t *len_inout)
{
    (void)out;
    (void)len_inout;
    return BB_ERR_NOT_FOUND;
}

void bb_diag_panic_clear(void)
{
}

bool bb_diag_panic_coredump_available(void)
{
    return false;
}

bb_err_t bb_diag_panic_coredump_get(bb_diag_panic_summary_t *out)
{
    (void)out;
    return BB_ERR_NOT_FOUND;
}

uint32_t bb_diag_panic_boots_since(void)
{
    return 0;
}

size_t bb_diag_panic_coredump_size(void) { return 0; }

bb_err_t bb_diag_panic_coredump_read_bytes(uint8_t *buf, size_t max_len, size_t *out_len)
{
    (void)buf; (void)max_len;
    if (out_len) *out_len = 0;
    return BB_ERR_NOT_FOUND;
}

bb_err_t bb_diag_panic_app_sha(char *out, size_t out_size)
{
    if (!out || out_size == 0) return BB_ERR_INVALID_ARG;
    out[0] = '\0';
    return BB_ERR_NOT_FOUND;
}

void bb_diag_panic_coredump_erase(void) {}

uint32_t bb_diag_abnormal_reset_count(void) { return 0; }
void bb_diag_abnormal_reset_count_clear(void) {}

size_t bb_diag_panic_order_copy(const char *buf, size_t buf_size,
                                 size_t length, size_t write_pos,
                                 char *out, size_t out_cap)
{
    if (!buf || !out || buf_size == 0 || out_cap == 0) {
        if (out && out_cap > 0) out[0] = '\0';
        return 0;
    }

    size_t to_copy = (length < out_cap - 1) ? length : (out_cap - 1);

    if (length == buf_size) {
        // Buffer full and wrapped: oldest byte is at write_pos
        size_t first_chunk = buf_size - write_pos;
        if (first_chunk > to_copy) first_chunk = to_copy;

        memcpy(out, &buf[write_pos], first_chunk);

        size_t second_chunk = to_copy - first_chunk;
        if (second_chunk > 0) {
            memcpy(&out[first_chunk], buf, second_chunk);
        }
    } else {
        // Not wrapped: oldest byte is at index 0
        memcpy(out, buf, to_copy);
    }

    out[to_copy] = '\0';
    return to_copy;
}
