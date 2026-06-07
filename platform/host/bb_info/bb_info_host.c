#include "bb_info.h"

#include <stdlib.h>
#include <string.h>

#include "../../components/bb_info/bb_info_schema_priv.h"

#ifdef BB_INFO_TESTING
#include "bb_info_test.h"
#endif

#define BB_INFO_MAX_EXTENDERS 4
#define BB_HEALTH_MAX_EXTENDERS 4

typedef struct {
    bb_info_extender_fn fn;
    const char         *schema_props;
} bb_info_extender_entry_t;

static bb_info_extender_entry_t s_extenders[BB_INFO_MAX_EXTENDERS];
static int   s_extender_count = 0;
static bool  s_frozen         = false;

// Capability registry (host twin)
static const char *s_capabilities[BB_INFO_MAX_CAPABILITIES];
static int         s_capability_count = 0;

static bb_info_extender_entry_t s_health_extenders[BB_HEALTH_MAX_EXTENDERS];
static int   s_health_extender_count = 0;

// Assembled info schema (lazily built on first call to bb_info_get_assembled_schema).
static char *s_assembled_info_schema = NULL;
static bool  s_assembled_info_done   = false;

// Assembled health extender schema (lazily built on first call).
static char *s_assembled_health_schema = NULL;
static bool  s_assembled_health_done   = false;

bb_err_t bb_info_register_extender_ex(bb_info_extender_fn fn,
                                       const char *schema_props_fragment)
{
    if (!fn) return BB_ERR_INVALID_ARG;
    if (s_frozen) return BB_ERR_INVALID_STATE;
    if (s_extender_count >= BB_INFO_MAX_EXTENDERS) return BB_ERR_NO_SPACE;
    const char *frag = (schema_props_fragment && schema_props_fragment[0]) ? schema_props_fragment : NULL;
    s_extenders[s_extender_count].fn           = fn;
    s_extenders[s_extender_count].schema_props  = frag;
    s_extender_count++;
    return BB_OK;
}

bb_err_t bb_info_register_extender(bb_info_extender_fn fn)
{
    return bb_info_register_extender_ex(fn, NULL);
}

bb_err_t bb_health_register_extender_ex(bb_info_extender_fn fn,
                                         const char *schema_props_fragment)
{
    if (!fn) return BB_ERR_INVALID_ARG;
    if (s_frozen) return BB_ERR_INVALID_STATE;
    if (s_health_extender_count >= BB_HEALTH_MAX_EXTENDERS) return BB_ERR_NO_SPACE;
    const char *frag = (schema_props_fragment && schema_props_fragment[0]) ? schema_props_fragment : NULL;
    s_health_extenders[s_health_extender_count].fn           = fn;
    s_health_extenders[s_health_extender_count].schema_props  = frag;
    s_health_extender_count++;
    return BB_OK;
}

bb_err_t bb_health_register_extender(bb_info_extender_fn fn)
{
    return bb_health_register_extender_ex(fn, NULL);
}

void bb_info_register_capability(const char *name)
{
    if (!name || !name[0]) return;
    if (s_frozen) {
        /* post-freeze: ignore + log (bb_log_w maps to fprintf on host) */
        return;
    }
    for (int i = 0; i < s_capability_count; i++) {
        if (strcmp(s_capabilities[i], name) == 0) return;
    }
    if (s_capability_count >= BB_INFO_MAX_CAPABILITIES) return;
    s_capabilities[s_capability_count++] = name;
}

#ifdef BB_INFO_TESTING

void bb_info_freeze_for_test(void)
{
    s_frozen = true;
}

void bb_info_invoke_extenders_for_test(void *root)
{
    for (int i = 0; i < s_extender_count; i++) {
        s_extenders[i].fn(root);
    }
}

void bb_health_invoke_extenders_for_test(void *root)
{
    for (int i = 0; i < s_health_extender_count; i++) {
        s_health_extenders[i].fn(root);
    }
}

const char *bb_health_get_assembled_extender_schema(void)
{
    if (s_assembled_health_done) return s_assembled_health_schema;

    /* Compute length of all health extender fragments joined by commas. */
    size_t len = 1; /* NUL terminator */
    bool first = true;
    for (int i = 0; i < s_health_extender_count; i++) {
        if (s_health_extenders[i].schema_props) {
            if (!first) len += 1; /* comma */
            len += strlen(s_health_extenders[i].schema_props);
            first = false;
        }
    }

    char *buf = malloc(len);
    if (buf) {
        char *p = buf;
        bool first2 = true;
        for (int i = 0; i < s_health_extender_count; i++) {
            if (s_health_extenders[i].schema_props) {
                if (!first2) *p++ = ',';
                p = stpcpy(p, s_health_extenders[i].schema_props);
                first2 = false;
            }
        }
        *p = '\0';
    }

    s_assembled_health_schema = buf;
    s_assembled_health_done   = true;
    return s_assembled_health_schema;
}

void bb_info_reset_for_test(void)
{
    memset(s_extenders, 0, sizeof(s_extenders));
    s_extender_count = 0;
    memset(s_health_extenders, 0, sizeof(s_health_extenders));
    s_health_extender_count = 0;
    memset(s_capabilities, 0, sizeof(s_capabilities));
    s_capability_count = 0;
    s_frozen = false;
    free(s_assembled_info_schema);
    s_assembled_info_schema = NULL;
    s_assembled_info_done   = false;
    free(s_assembled_health_schema);
    s_assembled_health_schema = NULL;
    s_assembled_health_done   = false;
}

const char *bb_info_get_assembled_schema(void)
{
    if (s_assembled_info_done) return s_assembled_info_schema;

    size_t len = strlen(k_info_schema_base) + strlen(k_info_schema_suffix) + 1;
    for (int i = 0; i < s_extender_count; i++) {
        if (s_extenders[i].schema_props) {
            len += 1 + strlen(s_extenders[i].schema_props); // comma + fragment
        }
    }

    char *buf = malloc(len);
    if (buf) {
        char *p = buf;
        p = stpcpy(p, k_info_schema_base);
        for (int i = 0; i < s_extender_count; i++) {
            if (s_extenders[i].schema_props) {
                *p++ = ',';
                p = stpcpy(p, s_extenders[i].schema_props);
            }
        }
        stpcpy(p, k_info_schema_suffix);
    }

    s_assembled_info_schema = buf;
    s_assembled_info_done   = true;
    return s_assembled_info_schema;
}

#endif /* BB_INFO_TESTING */
