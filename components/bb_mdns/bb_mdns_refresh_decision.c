#include "bb_mdns_refresh_decision.h"

bool bb_mdns_refresh_should_recreate(bb_mdns_refresh_delete_rc_t rc)
{
    /* Recreate is safe unless the delete enqueue itself failed for lack of
     * memory/queue space (BB_MDNS_REFRESH_DELETE_NO_MEM) — OK and NOT_FOUND
     * both mean the old browse is gone (or already was), so it's safe to
     * seed a fresh one. */
    return rc != BB_MDNS_REFRESH_DELETE_NO_MEM;
}
