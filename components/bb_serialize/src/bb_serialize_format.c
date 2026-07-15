// Format-dispatch registry -- a thin bb_registry consumer keyed by
// bb_format_name(fmt). See bb_serialize_format.h for the seam contract.
#include "bb_serialize_format.h"

#include "bb_log.h"
#include "bb_registry.h"

static const char *TAG = "bb_serialize_format";

// Small, fixed capacity -- one slot per real bb_format_t value plus
// headroom; format backends are a handful of link-time-known TUs, not an
// open-ended runtime set.
BB_REGISTRY_DEFINE_TAGGED(s_bb_serialize_format_registry, 8, "bb_serialize_format");

bb_err_t bb_serialize_format_register(bb_format_t fmt, const bb_serialize_format_entry_t *entry)
{
    if (!entry) return BB_ERR_INVALID_ARG;

    const char *name = bb_format_name(fmt);
    if (!name) return BB_ERR_INVALID_ARG;  // BB_FORMAT_NONE or out-of-range

    const bb_serialize_format_entry_t *existing =
        (const bb_serialize_format_entry_t *)bb_registry_lookup(&s_bb_serialize_format_registry, name);
    if (!existing) {
        return bb_registry_register(&s_bb_serialize_format_registry, name, (void *)entry);
    }

    // Identical re-register (same backend vtables) is the legitimate
    // idempotent codegen re-run -- no-op. A different backend claiming an
    // already-registered format is a composition bug, not last-writer-wins:
    // reject it loudly rather than silently clobbering the prior entry.
    if (existing->emit == entry->emit && existing->parse == entry->parse) {
        return BB_OK;
    }

    bb_log_w(TAG, "format '%s' re-registered with a different backend", name);
    return BB_ERR_INVALID_STATE;
}

static const bb_serialize_format_entry_t *bb_serialize_format_lookup(bb_format_t fmt)
{
    const char *name = bb_format_name(fmt);
    if (!name) return NULL;

    return (const bb_serialize_format_entry_t *)bb_registry_lookup(&s_bb_serialize_format_registry, name);
}

const bb_serialize_emit_t *bb_serialize_format_get_emit(bb_format_t fmt)
{
    const bb_serialize_format_entry_t *entry = bb_serialize_format_lookup(fmt);
    return entry ? entry->emit : NULL;
}

const void *bb_serialize_format_get_parse(bb_format_t fmt)
{
    const bb_serialize_format_entry_t *entry = bb_serialize_format_lookup(fmt);
    return entry ? entry->parse : NULL;
}

#ifdef BB_SERIALIZE_FORMAT_TESTING
void bb_serialize_format_test_reset(void)
{
    bb_registry_reset(&s_bb_serialize_format_registry);
}
#endif
