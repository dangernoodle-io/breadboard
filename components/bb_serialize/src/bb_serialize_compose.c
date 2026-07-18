// Stateless composition driver -- a loop over a caller-supplied entries[]
// array, not nested recursion. See bb_serialize_compose.h for the
// flat-stack property this structure guarantees.

#include "bb_serialize_compose.h"

bb_err_t bb_serialize_compose_walk(const bb_serialize_compose_entry_t *entries, size_t n,
                                    bb_serialize_compose_shape_t shape,
                                    const bb_serialize_emit_t *emit)
{
    if (!emit) return BB_ERR_INVALID_ARG;
    if (!entries && n) return BB_ERR_INVALID_ARG;

    for (size_t i = 0; i < n; i++) {
        const bb_serialize_compose_entry_t *e = &entries[i];
        if (!e->desc) return BB_ERR_INVALID_ARG;

        if (e->gather) {
            bb_err_t err = e->gather(e->snap, e->ctx);
            if (err != BB_OK) return err;
        }

        if (shape != BB_SERIALIZE_COMPOSE_RAW) {
            emit->begin_obj(emit->ctx, shape == BB_SERIALIZE_COMPOSE_OBJECT ? e->name : NULL);
        }

        bb_serialize_walk(e->desc, e->snap, emit);

        if (shape != BB_SERIALIZE_COMPOSE_RAW) {
            emit->end_obj(emit->ctx);
        }
    }

    return BB_OK;
}
