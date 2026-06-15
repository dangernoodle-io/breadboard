// bb_section — reusable named-section registry.
// Compiled on both host (test) and ESP-IDF.
#include "bb_section.h"
#include "bb_log.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

bb_err_t bb_section_register(bb_section_registry_t *reg,
                              const char *name,
                              bb_section_get_fn get,
                              bb_section_patch_fn patch,
                              void *ctx,
                              const char *schema_props)
{
    if (!reg || !name || !get) return BB_ERR_INVALID_ARG;
    if (reg->frozen)           return BB_ERR_INVALID_STATE;
    if (reg->count >= BB_SECTION_MAX) return BB_ERR_NO_SPACE;

    bb_section_entry_t *e = &reg->entries[reg->count];
    e->name         = name;
    e->get          = get;
    e->patch        = patch;
    e->ctx          = ctx;
    e->schema_props = (schema_props && schema_props[0]) ? schema_props : NULL;
    reg->count++;

    if (reg->tag) {
        bb_log_d(reg->tag, "registered section '%s' (%s)", name,
                 patch ? "rw" : "ro");
    }
    return BB_OK;
}

void bb_section_build_get(const bb_section_registry_t *reg, bb_json_t root)
{
    if (!reg) return;
    for (int i = 0; i < reg->count; i++) {
        const bb_section_entry_t *e = &reg->entries[i];
        bb_json_t child = bb_json_obj_new();
        e->get(child, e->ctx);
        bb_json_obj_set_obj(root, e->name, child);
    }
}

bb_err_t bb_section_dispatch_patch(const bb_section_registry_t *reg, bb_json_t body)
{
    if (!reg) return BB_ERR_INVALID_ARG;
    for (int i = 0; i < reg->count; i++) {
        const bb_section_entry_t *e = &reg->entries[i];
        bb_json_t child = bb_json_obj_get_item(body, e->name);
        if (!child) continue;
        if (!e->patch) {
            if (reg->tag) {
                bb_log_w(reg->tag, "PATCH on read-only section '%s'", e->name);
            }
            return BB_ERR_INVALID_ARG;
        }
        bb_err_t rc = e->patch(child, e->ctx);
        if (rc != BB_OK) return rc;
    }
    return BB_OK;
}

void bb_section_freeze(bb_section_registry_t *reg)
{
    if (reg) reg->frozen = true;
}

char *bb_section_assemble_schema(const bb_section_registry_t *reg,
                                 const char *base_prefix,
                                 const char *base_suffix)
{
    if (!base_prefix || !base_suffix) return NULL;

    // Compute length: prefix + ("\"<name>\":<props>" per section with props, comma-separated) + suffix + NUL
    size_t len = strlen(base_prefix) + strlen(base_suffix) + 1;
    bool first = true;
    if (reg) {
        for (int i = 0; i < reg->count; i++) {
            const bb_section_entry_t *e = &reg->entries[i];
            if (!e->schema_props) continue;
            // ,"<name>":<schema_props>  or  "<name>":<schema_props> for first
            len += 1 + 1 + strlen(e->name) + 1 + 1 + strlen(e->schema_props); // ,"<name>":props
        }
    }

    char *buf = malloc(len);
    if (!buf) return NULL;  // LCOV_EXCL_LINE

    char *p = buf;
    p = stpcpy(p, base_prefix);
    if (reg) {
        for (int i = 0; i < reg->count; i++) {
            const bb_section_entry_t *e = &reg->entries[i];
            if (!e->schema_props) continue;
            if (!first) *p++ = ',';
            first = false;
            *p++ = '"';
            p = stpcpy(p, e->name);
            *p++ = '"';
            *p++ = ':';
            p = stpcpy(p, e->schema_props);
        }
    }
    stpcpy(p, base_suffix);

    return buf;
}
