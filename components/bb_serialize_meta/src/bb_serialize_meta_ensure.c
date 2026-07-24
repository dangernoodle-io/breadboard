// bb_serialize_meta_ensure — bb_serialize_meta_ensure_composed(), the
// shared "compose once at init, idempotently" dispatch helper factored out
// of the 9 compose-at-init call sites (B1-1204) that each hand-rolled their
// own `if (already-composed) return BB_OK;` guard around a composer call.
// See bb_serialize_meta.h's doc comment for the full contract.

#include "bb_serialize_meta.h"

bb_err_t bb_serialize_meta_ensure_composed(bb_serialize_meta_composer_fn   composer,
                                            const bb_serialize_desc_t      *desc,
                                            const bb_serialize_desc_meta_t *meta,
                                            char *buf, size_t buf_size)
{
    if (buf[0] != '\0') return BB_OK;  // idempotent sentinel on caller-owned buf

    size_t n = 0;
    return composer(desc, meta, buf, buf_size, &n);
}
