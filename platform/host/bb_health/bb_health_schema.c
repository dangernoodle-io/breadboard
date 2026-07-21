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

#include "bb_log.h"

#include <stdlib.h>
#include <string.h>

#ifdef BB_HEALTH_TESTING
// Test-only malloc override (mirrors bb_response_set_malloc()) -- lets a
// host test force the OOM path below without actually exhausting memory.
static void *(*s_malloc_fn)(size_t) = NULL;
void bb_health_schema_set_malloc(void *(*m)(size_t)) { s_malloc_fn = m; }
static void *schema_malloc(size_t sz) { return s_malloc_fn ? s_malloc_fn(sz) : malloc(sz); }
#else
static void *schema_malloc(size_t sz) { return malloc(sz); }
#endif

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
