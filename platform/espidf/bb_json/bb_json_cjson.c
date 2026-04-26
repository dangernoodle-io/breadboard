#include "bb_json.h"

#ifndef ESP_PLATFORM
#include "bb_json_test_hooks.h"
#endif

#include <stdlib.h>
#include <string.h>
#include "cJSON.h"

// ---------------------------------------------------------------------------
// Fault injection (host-only test hook)
// ---------------------------------------------------------------------------

#ifndef ESP_PLATFORM

static int s_alloc_fail_after = -1;  // -1 = disabled

void bb_json_host_force_alloc_fail_after(int n)
{
    s_alloc_fail_after = n;
}

// Returns true if this alloc should be forced to fail.
static bool should_fail_alloc(void)
{
    if (s_alloc_fail_after < 0) return false;
    if (s_alloc_fail_after == 0) {
        s_alloc_fail_after = -1;  // auto-reset after one failure
        return true;
    }
    s_alloc_fail_after--;
    return false;
}

#endif  // !ESP_PLATFORM

// ---------------------------------------------------------------------------
// Builders
// ---------------------------------------------------------------------------

bb_json_t bb_json_obj_new(void)
{
#ifndef ESP_PLATFORM
    if (should_fail_alloc()) return NULL;
#endif
    return (bb_json_t)cJSON_CreateObject();
}

bb_json_t bb_json_arr_new(void)
{
#ifndef ESP_PLATFORM
    if (should_fail_alloc()) return NULL;
#endif
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

// ---------------------------------------------------------------------------
// Raw item access
// ---------------------------------------------------------------------------

bb_json_t bb_json_obj_get_item(bb_json_t obj, const char *key)
{
    if (!obj || !key) return NULL;
    return (bb_json_t)cJSON_GetObjectItem((cJSON *)obj, key);
}

int bb_json_arr_size(bb_json_t arr)
{
    if (!arr) return 0;
    return cJSON_GetArraySize((cJSON *)arr);
}

bb_json_t bb_json_arr_get_item(bb_json_t arr, int idx)
{
    if (!arr) return NULL;
    return (bb_json_t)cJSON_GetArrayItem((cJSON *)arr, idx);
}

// ---------------------------------------------------------------------------
// Item type predicates
// ---------------------------------------------------------------------------

bool bb_json_item_is_true(bb_json_t item)
{
    return item && cJSON_IsTrue((cJSON *)item);
}

bool bb_json_item_is_null(bb_json_t item)
{
    return !item || cJSON_IsNull((cJSON *)item);
}

bool bb_json_item_is_number(bb_json_t item)
{
    return item && cJSON_IsNumber((cJSON *)item);
}

bool bb_json_item_is_string(bb_json_t item)
{
    return item && cJSON_IsString((cJSON *)item);
}

bool bb_json_item_is_array(bb_json_t item)
{
    return item && cJSON_IsArray((cJSON *)item);
}

bool bb_json_item_is_object(bb_json_t item)
{
    return item && cJSON_IsObject((cJSON *)item);
}

// ---------------------------------------------------------------------------
// Item value accessors
// ---------------------------------------------------------------------------

const char *bb_json_item_get_string(bb_json_t item)
{
    if (!item) return NULL;
    return ((cJSON *)item)->valuestring;
}

double bb_json_item_get_double(bb_json_t item)
{
    if (!item) return 0.0;
    return ((cJSON *)item)->valuedouble;
}

int bb_json_item_get_int(bb_json_t item)
{
    if (!item) return 0;
    return ((cJSON *)item)->valueint;
}

char *bb_json_item_serialize(bb_json_t item)
{
    if (!item) return NULL;
    return cJSON_PrintUnformatted((cJSON *)item);
}

// ---------------------------------------------------------------------------
// Raw JSON injection
// ---------------------------------------------------------------------------

void bb_json_obj_set_raw(bb_json_t obj, const char *key, const char *json_literal)
{
    if (!obj || !key) return;
    cJSON *parsed = json_literal ? cJSON_Parse(json_literal) : NULL;
    if (!parsed) {
        cJSON_AddNullToObject((cJSON *)obj, key);
        return;
    }
    cJSON_AddItemToObject((cJSON *)obj, key, parsed);
}
