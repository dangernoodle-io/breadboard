#include "bb_openapi.h"
#include "bb_log.h"

#include <cJSON.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>

static const char *TAG = "bb_openapi_validate";

// ---------------------------------------------------------------------------
// Path helpers
// ---------------------------------------------------------------------------
// Maximum path depth supported by the path stack.
#define PATH_DEPTH_MAX 16

typedef struct {
    char segments[PATH_DEPTH_MAX][64];
    int  depth;
} path_stack_t;

// Render the current path stack into buf as a dotted string.
// Empty (depth == 0) yields "".
static void path_render(const path_stack_t *ps, char *buf, size_t bufsz)
{
    buf[0] = '\0';
    size_t written = 0;
    for (int i = 0; i < ps->depth && written < bufsz - 1; i++) {
        if (i > 0) {
            buf[written++] = '.';
            buf[written] = '\0';
        }
        size_t seg_len = strlen(ps->segments[i]);
        size_t avail = bufsz - written - 1;
        if (seg_len > avail) seg_len = avail;
        memcpy(buf + written, ps->segments[i], seg_len);
        written += seg_len;
        buf[written] = '\0';
    }
}

static void path_push(path_stack_t *ps, const char *seg)
{
    if (ps->depth >= PATH_DEPTH_MAX) return;
    strncpy(ps->segments[ps->depth], seg, sizeof(ps->segments[0]) - 1);
    ps->segments[ps->depth][sizeof(ps->segments[0]) - 1] = '\0';
    ps->depth++;
}

static void path_push_index(path_stack_t *ps, int idx)
{
    if (ps->depth >= PATH_DEPTH_MAX) return;
    snprintf(ps->segments[ps->depth], sizeof(ps->segments[0]), "[%d]", idx);
    ps->depth++;
}

static void path_pop(path_stack_t *ps)
{
    if (ps->depth > 0) ps->depth--;
}

// ---------------------------------------------------------------------------
// Type check helpers
// ---------------------------------------------------------------------------

static bool value_matches_type(const cJSON *value, const char *type)
{
    if (strcmp(type, "string") == 0)  return cJSON_IsString(value);
    if (strcmp(type, "integer") == 0) return cJSON_IsNumber(value);
    if (strcmp(type, "number") == 0)  return cJSON_IsNumber(value);
    if (strcmp(type, "boolean") == 0) return cJSON_IsBool(value);
    if (strcmp(type, "null") == 0)    return cJSON_IsNull(value);
    if (strcmp(type, "object") == 0)  return cJSON_IsObject(value);
    if (strcmp(type, "array") == 0)   return cJSON_IsArray(value);

    bb_log_w(TAG, "unknown JSON Schema type value '%s' — ignored", type);
    return true;
}

// ---------------------------------------------------------------------------
// Forward declaration for recursion
// ---------------------------------------------------------------------------

static bb_err_t validate_node(const cJSON *schema, const cJSON *value,
                              path_stack_t *ps,
                              bb_openapi_validate_err_t *err);

// ---------------------------------------------------------------------------
// Keyword handlers
// ---------------------------------------------------------------------------

static bb_err_t check_type(const cJSON *schema, const cJSON *value,
                           path_stack_t *ps, bb_openapi_validate_err_t *err)
{
    cJSON *type_node = cJSON_GetObjectItemCaseSensitive(schema, "type");
    if (!type_node) return BB_OK;
    if (!cJSON_IsString(type_node)) return BB_OK;  // malformed keyword — ignore

    const char *type = type_node->valuestring;
    if (!value_matches_type(value, type)) {
        if (err) {
            path_render(ps, err->path, sizeof(err->path));
            snprintf(err->message, sizeof(err->message),
                     "expected type '%s' but got cJSON type %d", type, value->type);
        }
        return BB_ERR_VALIDATION;
    }
    return BB_OK;
}

static bb_err_t check_enum(const cJSON *schema, const cJSON *value,
                           path_stack_t *ps, bb_openapi_validate_err_t *err)
{
    cJSON *enum_node = cJSON_GetObjectItemCaseSensitive(schema, "enum");
    if (!enum_node) return BB_OK;
    if (!cJSON_IsArray(enum_node)) return BB_OK;

    const cJSON *entry;
    for (entry = enum_node->child; entry != NULL; entry = entry->next) {
        if (cJSON_Compare(value, entry, true)) return BB_OK;
    }

    if (err) {
        path_render(ps, err->path, sizeof(err->path));
        char val_str[64] = "<non-string>";
        if (cJSON_IsString(value)) {
            strncpy(val_str, value->valuestring, sizeof(val_str) - 1);
            val_str[sizeof(val_str) - 1] = '\0';
        } else if (cJSON_IsNumber(value)) {
            snprintf(val_str, sizeof(val_str), "%g", value->valuedouble);
        }
        snprintf(err->message, sizeof(err->message),
                 "value '%s' is not in enum", val_str);
    }
    return BB_ERR_VALIDATION;
}

static bb_err_t check_required(const cJSON *schema, const cJSON *value,
                               path_stack_t *ps, bb_openapi_validate_err_t *err)
{
    cJSON *req_node = cJSON_GetObjectItemCaseSensitive(schema, "required");
    if (!req_node) return BB_OK;
    if (!cJSON_IsArray(req_node)) return BB_OK;
    if (!cJSON_IsObject(value)) return BB_OK;  // type check handles this separately

    const cJSON *req_key;
    for (req_key = req_node->child; req_key != NULL; req_key = req_key->next) {
        if (!cJSON_IsString(req_key)) continue;
        const char *key_name = req_key->valuestring;
        if (!cJSON_HasObjectItem(value, key_name)) {
            if (err) {
                path_render(ps, err->path, sizeof(err->path));
                snprintf(err->message, sizeof(err->message),
                         "required property '%s' is missing", key_name);
            }
            return BB_ERR_VALIDATION;
        }
    }
    return BB_OK;
}

static bb_err_t check_properties(const cJSON *schema, const cJSON *value,
                                 path_stack_t *ps, bb_openapi_validate_err_t *err)
{
    cJSON *props = cJSON_GetObjectItemCaseSensitive(schema, "properties");
    if (!props) return BB_OK;
    if (!cJSON_IsObject(props)) return BB_OK;
    if (!cJSON_IsObject(value)) return BB_OK;

    // Check additionalProperties: false
    cJSON *add_props = cJSON_GetObjectItemCaseSensitive(schema, "additionalProperties");
    bool reject_extra = add_props && cJSON_IsBool(add_props) && !cJSON_IsTrue(add_props);

    if (reject_extra) {
        const cJSON *val_key;
        for (val_key = value->child; val_key != NULL; val_key = val_key->next) {
            if (!cJSON_GetObjectItemCaseSensitive(props, val_key->string)) {
                if (err) {
                    path_render(ps, err->path, sizeof(err->path));
                    snprintf(err->message, sizeof(err->message),
                             "additional property '%s' not allowed", val_key->string);
                }
                return BB_ERR_VALIDATION;
            }
        }
    }

    // Recurse into declared properties that exist in the value
    const cJSON *prop_schema;
    for (prop_schema = props->child; prop_schema != NULL; prop_schema = prop_schema->next) {
        const char *key = prop_schema->string;
        cJSON *val_child = cJSON_GetObjectItemCaseSensitive(value, key);
        if (!val_child) continue;  // missing required keys handled by check_required

        path_push(ps, key);
        bb_err_t rc = validate_node(prop_schema, val_child, ps, err);
        path_pop(ps);
        if (rc != BB_OK) return rc;
    }
    return BB_OK;
}

static bb_err_t check_items(const cJSON *schema, const cJSON *value,
                            path_stack_t *ps, bb_openapi_validate_err_t *err)
{
    cJSON *items_schema = cJSON_GetObjectItemCaseSensitive(schema, "items");
    if (!items_schema) return BB_OK;
    if (!cJSON_IsArray(value)) return BB_OK;

    int idx = 0;
    const cJSON *elem;
    for (elem = value->child; elem != NULL; elem = elem->next) {
        path_push_index(ps, idx);
        bb_err_t rc = validate_node(items_schema, elem, ps, err);
        path_pop(ps);
        if (rc != BB_OK) return rc;
        idx++;
    }
    return BB_OK;
}

// ---------------------------------------------------------------------------
// Unknown keyword warning
// ---------------------------------------------------------------------------

// Known keywords — anything else is warned once.
static const char *s_known_keywords[] = {
    "type", "properties", "required", "items", "enum", "additionalProperties",
    NULL
};

static bool is_known_keyword(const char *key)
{
    for (int i = 0; s_known_keywords[i]; i++) {
        if (strcmp(key, s_known_keywords[i]) == 0) return true;
    }
    return false;
}

static void warn_unknown_keywords(const cJSON *schema)
{
    const cJSON *kw;
    for (kw = schema->child; kw != NULL; kw = kw->next) {
        if (!is_known_keyword(kw->string)) {
            bb_log_w(TAG, "unknown JSON Schema keyword '%s' — ignored", kw->string);
        }
    }
}

// ---------------------------------------------------------------------------
// Core recursive validator
// ---------------------------------------------------------------------------

static bb_err_t validate_node(const cJSON *schema, const cJSON *value,
                              path_stack_t *ps,
                              bb_openapi_validate_err_t *err)
{
    if (!cJSON_IsObject(schema)) return BB_OK;

    warn_unknown_keywords(schema);

    bb_err_t rc;

    rc = check_type(schema, value, ps, err);
    if (rc != BB_OK) return rc;

    rc = check_enum(schema, value, ps, err);
    if (rc != BB_OK) return rc;

    rc = check_required(schema, value, ps, err);
    if (rc != BB_OK) return rc;

    rc = check_properties(schema, value, ps, err);
    if (rc != BB_OK) return rc;

    rc = check_items(schema, value, ps, err);
    if (rc != BB_OK) return rc;

    return BB_OK;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bb_err_t bb_openapi_validate(const char *schema_json,
                             const cJSON *value,
                             bb_openapi_validate_err_t *err)
{
    if (!schema_json || !value) return BB_ERR_INVALID_ARG;

    cJSON *schema = cJSON_Parse(schema_json);
    if (!schema) {
        bb_log_e(TAG, "schema_json failed to parse as JSON");
        return BB_ERR_INVALID_ARG;
    }

    path_stack_t ps;
    memset(&ps, 0, sizeof(ps));

    bb_err_t rc = validate_node(schema, value, &ps, err);

    cJSON_Delete(schema);
    return rc;
}
