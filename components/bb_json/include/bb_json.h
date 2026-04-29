#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Opaque JSON handle. Backend-specific underneath (cJSON* on ESP-IDF/host,
// heap struct wrapping DynamicJsonDocument on Arduino).
typedef void *bb_json_t;

// ---------------------------------------------------------------------------
// Builders
// ---------------------------------------------------------------------------

bb_json_t bb_json_obj_new(void);
bb_json_t bb_json_arr_new(void);

// ---------------------------------------------------------------------------
// Object setters — copy key + value into the tree
// ---------------------------------------------------------------------------

void bb_json_obj_set_string(bb_json_t obj, const char *key, const char *value);
void bb_json_obj_set_number(bb_json_t obj, const char *key, double value);
void bb_json_obj_set_bool  (bb_json_t obj, const char *key, bool value);
void bb_json_obj_set_null  (bb_json_t obj, const char *key);

// Transfer ownership of child to parent — caller must NOT bb_json_free the child.
void bb_json_obj_set_obj(bb_json_t obj, const char *key, bb_json_t child);
void bb_json_obj_set_arr(bb_json_t obj, const char *key, bb_json_t child);

// ---------------------------------------------------------------------------
// Array append
// ---------------------------------------------------------------------------

void bb_json_arr_append_string  (bb_json_t arr, const char *value);
void bb_json_arr_append_string_n(bb_json_t arr, const char *str, size_t len);
void bb_json_arr_append_number  (bb_json_t arr, double value);

// Transfer ownership of child to parent — caller must NOT bb_json_free the child.
void bb_json_arr_append_obj(bb_json_t arr, bb_json_t child);

// ---------------------------------------------------------------------------
// Serialization
// ---------------------------------------------------------------------------

// Returns a malloc'd, NUL-terminated JSON string. Free with bb_json_free_str.
char *bb_json_serialize(bb_json_t root);

// Free a string returned by bb_json_serialize.
void bb_json_free_str(char *s);

// Free the entire JSON tree rooted at root.
void bb_json_free(bb_json_t root);

// ---------------------------------------------------------------------------
// Minimal read-side
// ---------------------------------------------------------------------------

// Parse a JSON text of length len (len==0 treats text as NUL-terminated).
// Returns NULL on parse error. Free with bb_json_free.
bb_json_t bb_json_parse(const char *text, size_t len);

// Named getters. Return true on success, false if key absent or wrong type.
bool bb_json_obj_get_string(bb_json_t obj, const char *key, char *out, size_t out_size);
bool bb_json_obj_get_number(bb_json_t obj, const char *key, double *out);
bool bb_json_obj_get_bool  (bb_json_t obj, const char *key, bool *out);

// Raw item access — returns the item handle (NULL if absent).
// The returned handle is owned by the parent; do NOT bb_json_free it.
bb_json_t bb_json_obj_get_item(bb_json_t obj, const char *key);

// Array helpers.
int       bb_json_arr_size    (bb_json_t arr);
bb_json_t bb_json_arr_get_item(bb_json_t arr, int idx);

// Item type predicates.
bool bb_json_item_is_true  (bb_json_t item);
bool bb_json_item_is_null  (bb_json_t item);
bool bb_json_item_is_number(bb_json_t item);
bool bb_json_item_is_string(bb_json_t item);
bool bb_json_item_is_array (bb_json_t item);
bool bb_json_item_is_object(bb_json_t item);

// Item value accessors (no type check — caller guards with bb_json_item_is_*).
const char *bb_json_item_get_string(bb_json_t item);  // pointer into tree; valid until bb_json_free
double      bb_json_item_get_double(bb_json_t item);
int         bb_json_item_get_int   (bb_json_t item);

// Serialize a single item (object, array, or scalar) to a malloc'd string.
// Free with bb_json_free_str.
char *bb_json_item_serialize(bb_json_t item);

// ---------------------------------------------------------------------------
// Raw JSON injection
// ---------------------------------------------------------------------------

// Parse json_literal as JSON and attach the resulting value under key in obj.
// json_literal must be a valid JSON text (object, array, or scalar).
// If json_literal is NULL or fails to parse, a JSON null is inserted instead.
// Ownership of the parsed value transfers to obj.
void bb_json_obj_set_raw(bb_json_t obj, const char *key, const char *json_literal);

// ---------------------------------------------------------------------------
// Walking
// ---------------------------------------------------------------------------

typedef enum { BB_JSON_KIND_OBJECT, BB_JSON_KIND_ARRAY, BB_JSON_KIND_OTHER } bb_json_kind_t;

// Determine if a document is an object, array, or something else.
bb_json_kind_t bb_json_get_kind(bb_json_t doc);

// Walk immediate children of a document (object or array).
// For objects, key is the key name (string); for arrays, key is NULL.
// Caller must not modify the doc during the walk.
void bb_json_walk_children(bb_json_t parent, void (*cb)(const char *key, bb_json_t child, void *ctx), void *ctx);

#ifdef __cplusplus
}
#endif
