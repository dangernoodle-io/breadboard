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

static bb_info_extender_entry_t s_health_extenders[BB_HEALTH_MAX_EXTENDERS];
static int   s_health_extender_count = 0;

// Assembled info schema (lazily built on first call to bb_info_get_assembled_schema).
static char *s_assembled_info_schema = NULL;
static bool  s_assembled_info_done   = false;

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

#ifdef BB_INFO_TESTING

void bb_info_freeze_for_test(void)
{
    s_frozen = true;
}

void bb_info_reset_for_test(void)
{
    memset(s_extenders, 0, sizeof(s_extenders));
    s_extender_count = 0;
    memset(s_health_extenders, 0, sizeof(s_health_extenders));
    s_health_extender_count = 0;
    s_frozen = false;
    free(s_assembled_info_schema);
    s_assembled_info_schema = NULL;
    s_assembled_info_done   = false;
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
