// Generic, pure, no-heap bb_serialize_arr_stream_t iterator over a
// caller-owned flat buffer. See bb_serialize.h for the full contract.

#include "bb_serialize.h"

#include <string.h>

static bool arr_buf_iter_next(void *iter_ctx, void *row_out)
{
    bb_serialize_arr_buf_iter_t *state = (bb_serialize_arr_buf_iter_t *)iter_ctx;
    if (state->idx >= state->count) return false;
    memcpy(row_out, state->base + state->idx * state->elem_size, state->elem_size);
    state->idx++;
    return true;
}

bb_serialize_arr_stream_t bb_serialize_arr_stream_from_buf(bb_serialize_arr_buf_iter_t *state,
                                                             const void *buf, size_t count,
                                                             size_t elem_size)
{
    state->base      = (const uint8_t *)buf;
    state->count     = count;
    state->elem_size = elem_size;
    state->idx       = 0;

    return (bb_serialize_arr_stream_t){
        .next     = arr_buf_iter_next,
        .iter_ctx = state,
        .row_size = elem_size,
    };
}
