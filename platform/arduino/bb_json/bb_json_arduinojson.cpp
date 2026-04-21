#include "bb_json.h"

#include <stdlib.h>
#include <string.h>
#include <ArduinoJson.h>

// ---------------------------------------------------------------------------
// Per-node heap struct
// Each bb_json_t is a bb_json_node_t*. For root nodes the DynamicJsonDocument
// owns the memory; child nodes that have been adopted into a parent document
// point at an element inside the parent's document (owned_doc == nullptr).
// ---------------------------------------------------------------------------

#ifndef BB_JSON_ARDUINO_DOC_BYTES
#define BB_JSON_ARDUINO_DOC_BYTES 512
#endif

struct bb_json_node_t {
    DynamicJsonDocument *owned_doc; // non-null only for root (owner) nodes
    JsonVariant          variant;   // the actual value inside the document
};

static bb_json_node_t *node_new_obj(void)
{
    DynamicJsonDocument *doc = new DynamicJsonDocument(BB_JSON_ARDUINO_DOC_BYTES);
    bb_json_node_t *n = new bb_json_node_t;
    n->owned_doc = doc;
    n->variant   = doc->to<JsonObject>();
    return n;
}

static bb_json_node_t *node_new_arr(void)
{
    DynamicJsonDocument *doc = new DynamicJsonDocument(BB_JSON_ARDUINO_DOC_BYTES);
    bb_json_node_t *n = new bb_json_node_t;
    n->owned_doc = doc;
    n->variant   = doc->to<JsonArray>();
    return n;
}

// Merge a child node into a target document slot.
// Serialise the child to a temp buffer and deserialise into the slot.
// This lets each subtree live in its own document while still producing
// correct JSON when the parent is serialised.
static void adopt_into(JsonVariant dst, bb_json_node_t *child)
{
    // Serialise child to string then deserialise into dst.
    // measureJson includes the NUL terminator in some versions; add 1 to be safe.
    size_t len = measureJson(child->variant) + 1;
    char *buf = (char *)malloc(len);
    if (!buf) return;
    serializeJson(child->variant, buf, len);
    deserializeJson(*child->owned_doc, buf); // re-parse is cheap for tiny docs
    // We can't easily do a deep-copy into dst's document without
    // serialise-then-deserialise into the *parent* doc.
    // Simpler approach: copy the raw JSON into dst via deserialisation.
    // ArduinoJson doesn't support cross-document moves, so we just copy.
    DynamicJsonDocument tmp(len + 32);
    deserializeJson(tmp, buf);
    dst.set(tmp.as<JsonVariant>());
    free(buf);
    // Free the child now that its data is in the parent.
    delete child->owned_doc;
    child->owned_doc = nullptr;
    delete child;
}

// ---------------------------------------------------------------------------
// C-linkage wrappers
// ---------------------------------------------------------------------------

extern "C" {

bb_json_t bb_json_obj_new(void)
{
    return (bb_json_t)node_new_obj();
}

bb_json_t bb_json_arr_new(void)
{
    return (bb_json_t)node_new_arr();
}

// ---------------------------------------------------------------------------
// Object setters
// ---------------------------------------------------------------------------

void bb_json_obj_set_string(bb_json_t obj, const char *key, const char *value)
{
    if (!obj || !key) return;
    bb_json_node_t *n = (bb_json_node_t *)obj;
    n->variant.as<JsonObject>()[key] = value ? value : "";
}

void bb_json_obj_set_number(bb_json_t obj, const char *key, double value)
{
    if (!obj || !key) return;
    bb_json_node_t *n = (bb_json_node_t *)obj;
    n->variant.as<JsonObject>()[key] = value;
}

void bb_json_obj_set_bool(bb_json_t obj, const char *key, bool value)
{
    if (!obj || !key) return;
    bb_json_node_t *n = (bb_json_node_t *)obj;
    n->variant.as<JsonObject>()[key] = value;
}

void bb_json_obj_set_null(bb_json_t obj, const char *key)
{
    if (!obj || !key) return;
    bb_json_node_t *n = (bb_json_node_t *)obj;
    n->variant.as<JsonObject>()[key] = (char *)nullptr;
}

void bb_json_obj_set_obj(bb_json_t obj, const char *key, bb_json_t child)
{
    if (!obj || !key || !child) return;
    bb_json_node_t *parent = (bb_json_node_t *)obj;
    bb_json_node_t *c      = (bb_json_node_t *)child;
    JsonVariant slot = parent->variant.as<JsonObject>().createNestedObject(key);
    adopt_into(slot, c);
}

void bb_json_obj_set_arr(bb_json_t obj, const char *key, bb_json_t child)
{
    if (!obj || !key || !child) return;
    bb_json_node_t *parent = (bb_json_node_t *)obj;
    bb_json_node_t *c      = (bb_json_node_t *)child;
    JsonVariant slot = parent->variant.as<JsonObject>().createNestedArray(key);
    adopt_into(slot, c);
}

// ---------------------------------------------------------------------------
// Array append
// ---------------------------------------------------------------------------

void bb_json_arr_append_string(bb_json_t arr, const char *value)
{
    if (!arr) return;
    bb_json_node_t *n = (bb_json_node_t *)arr;
    n->variant.as<JsonArray>().add(value ? value : "");
}

void bb_json_arr_append_number(bb_json_t arr, double value)
{
    if (!arr) return;
    bb_json_node_t *n = (bb_json_node_t *)arr;
    n->variant.as<JsonArray>().add(value);
}

void bb_json_arr_append_obj(bb_json_t arr, bb_json_t child)
{
    if (!arr || !child) return;
    bb_json_node_t *parent = (bb_json_node_t *)arr;
    bb_json_node_t *c      = (bb_json_node_t *)child;
    JsonVariant slot = parent->variant.as<JsonArray>().createNestedObject();
    adopt_into(slot, c);
}

// ---------------------------------------------------------------------------
// Serialization
// ---------------------------------------------------------------------------

char *bb_json_serialize(bb_json_t root)
{
    if (!root) return NULL;
    bb_json_node_t *n = (bb_json_node_t *)root;
    size_t len = measureJson(n->variant) + 1;
    char *buf = (char *)malloc(len);
    if (!buf) return NULL;
    serializeJson(n->variant, buf, len);
    return buf;
}

void bb_json_free_str(char *s)
{
    free(s);
}

void bb_json_free(bb_json_t root)
{
    if (!root) return;
    bb_json_node_t *n = (bb_json_node_t *)root;
    if (n->owned_doc) {
        delete n->owned_doc;
        n->owned_doc = nullptr;
    }
    delete n;
}

// ---------------------------------------------------------------------------
// Parse + getters
// ---------------------------------------------------------------------------

bb_json_t bb_json_parse(const char *text, size_t len)
{
    if (!text) return NULL;
    size_t doc_size = (len > 0 ? len : strlen(text)) * 2 + 64;
    DynamicJsonDocument *doc = new DynamicJsonDocument(doc_size);
    DeserializationError err;
    if (len > 0) {
        err = deserializeJson(*doc, text, len);
    } else {
        err = deserializeJson(*doc, text);
    }
    if (err) {
        delete doc;
        return NULL;
    }
    bb_json_node_t *n = new bb_json_node_t;
    n->owned_doc = doc;
    n->variant   = doc->as<JsonVariant>();
    return (bb_json_t)n;
}

bool bb_json_obj_get_string(bb_json_t obj, const char *key, char *out, size_t out_size)
{
    if (!obj || !key || !out || out_size == 0) return false;
    bb_json_node_t *n = (bb_json_node_t *)obj;
    JsonVariant v = n->variant.as<JsonObject>()[key];
    if (!v.is<const char *>()) return false;
    const char *s = v.as<const char *>();
    if (!s) return false;
    size_t src_len  = strlen(s);
    size_t copy_len = src_len < out_size - 1 ? src_len : out_size - 1;
    memcpy(out, s, copy_len);
    out[copy_len] = '\0';
    return true;
}

bool bb_json_obj_get_number(bb_json_t obj, const char *key, double *out)
{
    if (!obj || !key || !out) return false;
    bb_json_node_t *n = (bb_json_node_t *)obj;
    JsonVariant v = n->variant.as<JsonObject>()[key];
    if (!v.is<double>()) return false;
    *out = v.as<double>();
    return true;
}

bool bb_json_obj_get_bool(bb_json_t obj, const char *key, bool *out)
{
    if (!obj || !key || !out) return false;
    bb_json_node_t *n = (bb_json_node_t *)obj;
    JsonVariant v = n->variant.as<JsonObject>()[key];
    if (!v.is<bool>()) return false;
    *out = v.as<bool>();
    return true;
}

} // extern "C"
