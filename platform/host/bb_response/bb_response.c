// bb_response — reusable named-section registry.
// Compiled on both host (test) and ESP-IDF.
#include "bb_response.h"
#include "bb_log.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#ifdef BB_RESPONSE_TESTING
static void *(*s_malloc_fn)(size_t) = NULL;
void bb_response_set_malloc(void *(*m)(size_t)) { s_malloc_fn = m; }
static void *section_malloc(size_t sz) { return s_malloc_fn ? s_malloc_fn(sz) : malloc(sz); }
#else
static void *section_malloc(size_t sz) { return malloc(sz); }
#endif

bb_err_t bb_response_register(bb_response_registry_t *reg,
                              const char *name,
                              bb_response_get_fn get,
                              bb_response_patch_fn patch,
                              void *ctx,
                              const char *schema_props)
{
    if (!reg || !name || !get) return BB_ERR_INVALID_ARG;
    if (reg->frozen)           return BB_ERR_INVALID_STATE;
    int cap = (reg->cap > 0) ? reg->cap : BB_RESPONSE_MAX;
    if (cap > BB_RESPONSE_MAX) cap = BB_RESPONSE_MAX;
    if (reg->count >= cap) return BB_ERR_NO_SPACE;

    // Reject duplicate section names — avoids duplicate JSON keys and double
    // patch_fn dispatch.
    for (int i = 0; i < reg->count; i++) {
        if (strcmp(reg->entries[i].name, name) == 0) {
            if (reg->tag) {
                bb_log_w(reg->tag, "section '%s' already registered, ignoring duplicate", name);
            }
            return BB_ERR_INVALID_STATE;
        }
    }

    bb_response_entry_t *e = &reg->entries[reg->count];
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

void bb_response_build_get(const bb_response_registry_t *reg, bb_json_t root)
{
    if (!reg) return;
    for (int i = 0; i < reg->count; i++) {
        const bb_response_entry_t *e = &reg->entries[i];
        bb_json_t child = bb_json_obj_new();
        if (!child) continue;  // OOM: skip section rather than crash
        e->get(child, e->ctx);
        bb_json_obj_set_obj(root, e->name, child);
    }
}

bb_err_t bb_response_dispatch_patch(const bb_response_registry_t *reg, bb_json_t body)
{
    if (!reg) return BB_ERR_INVALID_ARG;

    // Pre-validation pass: reject any read-only section present in the body
    // BEFORE applying any patch_fn, so multi-section bodies are all-or-nothing.
    for (int i = 0; i < reg->count; i++) {
        const bb_response_entry_t *e = &reg->entries[i];
        bb_json_t child = bb_json_obj_get_item(body, e->name);
        if (!child) continue;
        if (!e->patch) {
            if (reg->tag) {
                bb_log_w(reg->tag, "PATCH on read-only section '%s' — rejecting before apply",
                         e->name);
            }
            return BB_ERR_INVALID_ARG;
        }
    }

    // Apply pass: all sections validated as writable above.
    for (int i = 0; i < reg->count; i++) {
        const bb_response_entry_t *e = &reg->entries[i];
        bb_json_t child = bb_json_obj_get_item(body, e->name);
        if (!child) continue;
        bb_err_t rc = e->patch(child, e->ctx);
        if (rc != BB_OK) return rc;
    }
    return BB_OK;
}

void bb_response_freeze(bb_response_registry_t *reg)
{
    if (reg) reg->frozen = true;
}

char *bb_response_assemble_schema(const bb_response_registry_t *reg,
                                 const char *base_prefix,
                                 const char *base_suffix)
{
    if (!base_prefix || !base_suffix) return NULL;

    // Compute length: prefix + ("\"<name>\":<props>" per section with props, comma-separated) + suffix + NUL
    size_t len = strlen(base_prefix) + strlen(base_suffix) + 1;
    bool first = true;
    if (reg) {
        for (int i = 0; i < reg->count; i++) {
            const bb_response_entry_t *e = &reg->entries[i];
            if (!e->schema_props) continue;
            // ,"<name>":<schema_props>  or  "<name>":<schema_props> for first
            len += 1 + 1 + strlen(e->name) + 1 + 1 + strlen(e->schema_props); // ,"<name>":props
        }
    }

    char *buf = section_malloc(len);
    if (!buf) return NULL;

    // True when the base already has object content (last char is not '{').
    // In that case every section needs a leading ',' — even the first one.
    size_t base_len = strlen(base_prefix);
    bool base_has_content = (base_len > 0 && base_prefix[base_len - 1] != '{');

    char *p = buf;
    p = stpcpy(p, base_prefix);
    if (reg) {
        for (int i = 0; i < reg->count; i++) {
            const bb_response_entry_t *e = &reg->entries[i];
            if (!e->schema_props) continue;
            if (base_has_content || !first) *p++ = ',';
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

char *bb_response_freeze_and_assemble(bb_response_registry_t *reg, const char *base, const char *suffix)
{
    bb_response_freeze(reg);
    char *s = bb_response_assemble_schema(reg, base, suffix);
    if (s == NULL) {
        bb_log_w("bb_response", "schema assembly: malloc failed; schema will be NULL");
    }
    return s;
}
