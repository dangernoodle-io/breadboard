// bb_health_schema -- assembles the /api/health 200 JSON-Schema from the
// bb_health_section composer registry (B1-1100, PR-5 of 6, epic B1-1054).
// Portable (no ESP-IDF/FreeRTOS types), compiled for host + device --
// mirrors bb_health_emit.c's portability (and its platform/host/ placement:
// this file's one-shot, init-time-only malloc() -- mirroring the retired
// bb_response_assemble_schema()'s own contract -- is legitimate here but
// would trip the raw-allocator lint under components/**, which does not
// scan platform/host/**; same precedent as bb_response.c itself). Replaces
// the retired bb_response_assemble_schema() call in bb_health_init().
#include "../../../components/bb_health/bb_health_schema_priv.h"
#include "../../../components/bb_health/bb_health_section_priv.h"
#include "../../../components/bb_health/bb_health_wire_priv.h"

#include "bb_log.h"
#include "bb_serialize_meta.h"

#include <stdlib.h>
#include <string.h>

#ifdef BB_HEALTH_TESTING
// Test-only malloc override (mirrors bb_response_set_malloc()) -- lets a
// host test force the OOM path below without actually exhausting memory.
static void *(*s_malloc_fn)(size_t) = NULL;
void bb_health_schema_set_malloc(void *(*m)(size_t)) { s_malloc_fn = m; }
static void *schema_malloc(size_t sz) { return s_malloc_fn ? s_malloc_fn(sz) : malloc(sz); }

// Test-only cap override for the config-ON fork's root_close_buf below
// (B1-1059 PR-d) -- the global BB_SERIALIZE_META_TESTING fail-injection
// seam (bb_serialize_meta_openapi_test_set_force_no_space()) can't isolate
// a root_close()-only failure from this function's EARLIER open_fragment()
// call (both share the same sticky flag, and open_fragment() runs first --
// forcing it fails-loud before root_close() is ever reached). This override
// shrinks root_close()'s OWN out_size argument so it hits its REAL (non-seam)
// BB_ERR_NO_SPACE overflow path on its own, independent of open_fragment().
// 0 = use the real sizeof(root_close_buf).
static size_t s_test_root_close_cap = 0;
void bb_health_schema_set_root_close_cap_for_test(size_t cap) { s_test_root_close_cap = cap; }
#else
static void *schema_malloc(size_t sz) { return malloc(sz); }
#endif

// ---------------------------------------------------------------------------
// CONFIG_BB_OPENAPI_RUNTIME_META (B1-1059 PR-d) -- gated DIRECTLY on this
// Kconfig symbol, never on BB_SERIALIZE_META_SHIP: that macro also covers
// BB_SERIALIZE_META_HOST (unconditionally set by the plain `native` host
// env, see platformio.ini), which must NOT flip this ROOT schema onto the
// runtime-compose path -- "meta tables compiled in for golden-testing"
// (SHIP) and "this route sources its schema at runtime" (RUNTIME_META) are
// deliberately distinct gates. Config OFF (default) is a zero-diff no-op:
// the `#else` arm below is byte-identical to the pre-PR-d function.
//
// Unlike every prior B1-1059 PR (one bb_serialize_meta_openapi_* composer
// call per section), this is the first ROOT composition: the "ok"/"network"
// root-identity slice (bb_health_wire_desc/_meta) is opened via
// bb_serialize_meta_openapi_open_fragment(), every registered section's
// schema_props is spliced in (same loop as the config-OFF path below), then
// the root is closed via bb_serialize_meta_openapi_root_close().
// ---------------------------------------------------------------------------
#if defined(CONFIG_BB_OPENAPI_RUNTIME_META)

char *bb_health_assemble_schema(void)
{
    char   root_open_buf[512];
    size_t root_open_len = 0;
    bb_err_t rc = bb_serialize_meta_openapi_open_fragment(&bb_health_wire_desc, &bb_health_wire_meta,
                                                            root_open_buf, sizeof(root_open_buf),
                                                            &root_open_len);
    if (rc != BB_OK) {
        bb_log_w("bb_health", "schema assembly: root open_fragment failed; schema will be NULL");
        return NULL;
    }

    // open_fragment() closes the root "properties" object as its LAST byte
    // (bb_oa_write_body(desc,meta,false,false) ends with the nested
    // "network" object's own closing "}}", the second of which closes
    // ROOT properties) -- strip it here to REOPEN root properties so the
    // per-section loop below can splice `,"<name>":<schema_props>` as
    // siblings of "ok"/"network" instead of as siblings of "properties"
    // itself (which, combined with root_close()'s
    // "additionalProperties":false, would reject every real /api/health
    // payload). This is the load-bearing step of this PR.
    if (root_open_len == 0 || root_open_buf[root_open_len - 1] != '}') {  // LCOV_EXCL_BR_LINE -- defensive:
        // open_fragment()'s contract (bb_oa_write_body(desc,meta,false,false), see
        // bb_serialize_meta_openapi.c) always ends in '}' for an OBJ root field
        // ("network"); this guard exists in case that contract ever changes, but is
        // not reachable given today's engine + bb_health_wire_desc/_meta shape.
        // LCOV_EXCL_START
        bb_log_w("bb_health", "schema assembly: root open_fragment shape unexpected; schema will be NULL");
        return NULL;
        // LCOV_EXCL_STOP
    }
    root_open_len--;

    char   root_close_buf[128];
    size_t root_close_cap = sizeof(root_close_buf);
#ifdef BB_HEALTH_TESTING
    // Never widen beyond the real buffer -- a test may only SHRINK the
    // effective cap to force a genuine overflow, never grow past
    // root_close_buf's actual stack allocation.
    if (s_test_root_close_cap && s_test_root_close_cap < root_close_cap) root_close_cap = s_test_root_close_cap;
#endif
    size_t root_close_len = 0;
    rc = bb_serialize_meta_openapi_root_close(&bb_health_wire_desc, &bb_health_wire_meta,
                                               root_close_buf, root_close_cap, &root_close_len);
    if (rc != BB_OK) {
        bb_log_w("bb_health", "schema assembly: root_close failed; schema will be NULL");
        return NULL;
    }

    uint16_t n = bb_health_section_count();

    // Length: (root_open_len, brace already stripped) + per-section
    // (,"<name>":<schema_props>) + re-added '}' (closes root properties
    // again) + root_close_len + NUL.
    size_t len = root_open_len + 1 + root_close_len + 1;
    for (uint16_t i = 0; i < n; i++) {
        const bb_health_section_t *sec = bb_health_section_get_by_index(i);
        if (!sec) continue;  // LCOV_EXCL_BR_LINE -- defensive: bb_health_section_get_by_index()
                              // never returns NULL for idx < bb_health_section_count() (its own
                              // registry contract, bb_health_section.c) -- same defensive class
                              // as the root open_fragment shape guard above.
        if (!sec->schema_props) continue;
        len += 1 + 1 + strlen(sec->name) + 1 + 1 + strlen(sec->schema_props);  // ,"<name>":<schema_props>
    }

    char *buf = schema_malloc(len);
    if (!buf) {
        bb_log_w("bb_health", "schema assembly: malloc failed; schema will be NULL");
        return NULL;
    }

    char *p = buf;
    memcpy(p, root_open_buf, root_open_len);
    p += root_open_len;
    for (uint16_t i = 0; i < n; i++) {
        const bb_health_section_t *sec = bb_health_section_get_by_index(i);
        if (!sec) continue;  // LCOV_EXCL_BR_LINE -- defensive: see the matching guard in the
                              // length-computation loop above.
        if (!sec->schema_props) continue;
        *p++ = ',';
        *p++ = '"';
        p = stpcpy(p, sec->name);
        *p++ = '"';
        *p++ = ':';
        p = stpcpy(p, sec->schema_props);
    }
    *p++ = '}';  // re-close root properties
    memcpy(p, root_close_buf, root_close_len);
    p += root_close_len;
    *p = '\0';

    return buf;
}

#else /* !CONFIG_BB_OPENAPI_RUNTIME_META */

char *bb_health_assemble_schema(void)
{
    uint16_t n = bb_health_section_count();

    // Length: base + suffix + (",\"<name>\":<schema_props>" per section with
    // schema_props) + NUL.
    size_t len = strlen(k_health_base) + strlen(k_health_suffix) + 1;
    for (uint16_t i = 0; i < n; i++) {
        const bb_health_section_t *sec = bb_health_section_get_by_index(i);
        if (!sec || !sec->schema_props) continue;
        len += 1 + 1 + strlen(sec->name) + 1 + 1 + strlen(sec->schema_props);  // ,"<name>":<schema_props>
    }

    char *buf = schema_malloc(len);
    if (!buf) {
        bb_log_w("bb_health", "schema assembly: malloc failed; schema will be NULL");
        return NULL;
    }

    // k_health_base always ends with non-'{' content (the network object's
    // closing braces), so every section carries an unconditional leading
    // comma -- no base-ends-with-'{' check needed (unlike
    // bb_response_assemble_schema()'s general-purpose version).
    char *p = stpcpy(buf, k_health_base);
    for (uint16_t i = 0; i < n; i++) {
        const bb_health_section_t *sec = bb_health_section_get_by_index(i);
        if (!sec || !sec->schema_props) continue;
        *p++ = ',';
        *p++ = '"';
        p = stpcpy(p, sec->name);
        *p++ = '"';
        *p++ = ':';
        p = stpcpy(p, sec->schema_props);
    }
    stpcpy(p, k_health_suffix);

    return buf;
}

#endif /* CONFIG_BB_OPENAPI_RUNTIME_META */
