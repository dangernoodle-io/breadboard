#include "bb_json.h"

#include <stdlib.h>
#include <string.h>
#include "cJSON.h"

// ---------------------------------------------------------------------------
// Builders
// ---------------------------------------------------------------------------

bb_json_t bb_json_obj_new(void)
{
    return (bb_json_t)cJSON_CreateObject();
}

bb_json_t bb_json_arr_new(void)
{
    return (bb_json_t)cJSON_CreateArray();
}

// ---------------------------------------------------------------------------
// Object setters
// ---------------------------------------------------------------------------

void bb_json_obj_set_string(bb_json_t obj, const char *key, const char *value)
{
    if (!obj || !key) return;
    cJSON_AddStringToObject((cJSON *)obj, key, value ? value : "");
}

void bb_json_obj_set_number(bb_json_t obj, const char *key, double value)
{
    if (!obj || !key) return;
    cJSON_AddNumberToObject((cJSON *)obj, key, value);
}

void bb_json_obj_set_bool(bb_json_t obj, const char *key, bool value)
{
    if (!obj || !key) return;
    cJSON_AddBoolToObject((cJSON *)obj, key, value);
}

void bb_json_obj_set_null(bb_json_t obj, const char *key)
{
    if (!obj || !key) return;
    cJSON_AddNullToObject((cJSON *)obj, key);
}

void bb_json_obj_set_obj(bb_json_t obj, const char *key, bb_json_t child)
{
    if (!obj || !key || !child) return;
    cJSON_AddItemToObject((cJSON *)obj, key, (cJSON *)child);
}

void bb_json_obj_set_arr(bb_json_t obj, const char *key, bb_json_t child)
{
    if (!obj || !key || !child) return;
    cJSON_AddItemToObject((cJSON *)obj, key, (cJSON *)child);
}

// ---------------------------------------------------------------------------
// Array append
// ---------------------------------------------------------------------------

void bb_json_arr_append_string(bb_json_t arr, const char *value)
{
    if (!arr) return;
    cJSON_AddItemToArray((cJSON *)arr, cJSON_CreateString(value ? value : ""));
}

void bb_json_arr_append_number(bb_json_t arr, double value)
{
    if (!arr) return;
    cJSON_AddItemToArray((cJSON *)arr, cJSON_CreateNumber(value));
}

void bb_json_arr_append_obj(bb_json_t arr, bb_json_t child)
{
    if (!arr || !child) return;
    cJSON_AddItemToArray((cJSON *)arr, (cJSON *)child);
}

// ---------------------------------------------------------------------------
// Serialization
// ---------------------------------------------------------------------------

char *bb_json_serialize(bb_json_t root)
{
    if (!root) return NULL;
    return cJSON_PrintUnformatted((cJSON *)root);
}

void bb_json_free_str(char *s)
{
    cJSON_free(s);
}

void bb_json_free(bb_json_t root)
{
    if (!root) return;
    cJSON_Delete((cJSON *)root);
}

// ---------------------------------------------------------------------------
// Parse + getters
// ---------------------------------------------------------------------------

bb_json_t bb_json_parse(const char *text, size_t len)
{
    if (!text) return NULL;
    // cJSON_ParseWithLength available in cJSON >= 1.7.14
    if (len > 0) {
        return (bb_json_t)cJSON_ParseWithLength(text, len);
    }
    return (bb_json_t)cJSON_Parse(text);
}

bool bb_json_obj_get_string(bb_json_t obj, const char *key, char *out, size_t out_size)
{
    if (!obj || !key || !out || out_size == 0) return false;
    cJSON *item = cJSON_GetObjectItemCaseSensitive((cJSON *)obj, key);
    if (!cJSON_IsString(item) || !item->valuestring) return false;
    size_t src_len = strlen(item->valuestring);
    size_t copy_len = src_len < out_size - 1 ? src_len : out_size - 1;
    memcpy(out, item->valuestring, copy_len);
    out[copy_len] = '\0';
    return true;
}

bool bb_json_obj_get_number(bb_json_t obj, const char *key, double *out)
{
    if (!obj || !key || !out) return false;
    cJSON *item = cJSON_GetObjectItemCaseSensitive((cJSON *)obj, key);
    if (!cJSON_IsNumber(item)) return false;
    *out = item->valuedouble;
    return true;
}

bool bb_json_obj_get_bool(bb_json_t obj, const char *key, bool *out)
{
    if (!obj || !key || !out) return false;
    cJSON *item = cJSON_GetObjectItemCaseSensitive((cJSON *)obj, key);
    if (!cJSON_IsBool(item)) return false;
    *out = cJSON_IsTrue(item);
    return true;
}
