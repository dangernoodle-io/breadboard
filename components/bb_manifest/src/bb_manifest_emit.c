#include "bb_manifest.h"
#include "bb_log.h"

#include <string.h>

static const char *TAG = "bb_manifest";

// Capacity limits
// NV_NAMESPACE_CAP counts entries, not unique namespaces.  Multiple callers
// can register distinct keys under the same namespace; each call is one entry.
// 16 entries supports up to ~2 callers × 8 logical namespaces.
#define NV_NAMESPACE_CAP 16
#define NV_KEYS_PER_NAMESPACE_CAP 32
#define MDNS_SERVICE_CAP 4
#define MDNS_KEYS_PER_SERVICE_CAP 16

// NVS namespace registry
typedef struct {
    const char *namespace;
    const bb_manifest_nv_t *keys;
    size_t n;
} nv_namespace_entry_t;

typedef struct {
    nv_namespace_entry_t entries[NV_NAMESPACE_CAP];
    size_t count;
} nv_registry_t;

// mDNS service registry
typedef struct {
    const char *service;
    const bb_manifest_mdns_t *keys;
    size_t n;
} mdns_service_entry_t;

typedef struct {
    mdns_service_entry_t entries[MDNS_SERVICE_CAP];
    size_t count;
} mdns_registry_t;

static nv_registry_t nv_reg = {0};
static mdns_registry_t mdns_reg = {0};

// ---------------------------------------------------------------------------
// Registry management
// ---------------------------------------------------------------------------

bb_err_t bb_manifest_register_nv(const char *namespace,
                                 const bb_manifest_nv_t *keys, size_t n)
{
    if (!namespace || !keys || n == 0) {
        return BB_ERR_INVALID_ARG;
    }

    // Check total key count doesn't exceed cap per namespace
    if (n > NV_KEYS_PER_NAMESPACE_CAP) {
        bb_log_e(TAG, "namespace %s: %zu keys exceeds cap %d",
                 namespace, n, NV_KEYS_PER_NAMESPACE_CAP);
        return BB_ERR_NO_SPACE;
    }

    // Walk existing entries for this namespace and validate no key-name
    // collisions.  Same namespace, different keys is fine (multiple callers).
    // Same namespace, same key name is a real bug.
    for (size_t i = 0; i < nv_reg.count; i++) {
        if (strcmp(nv_reg.entries[i].namespace, namespace) != 0) {
            continue;
        }
        // Namespace already has at least one entry — check for key collisions.
        for (size_t ei = 0; ei < nv_reg.entries[i].n; ei++) {
            for (size_t ni = 0; ni < n; ni++) {
                if (strcmp(nv_reg.entries[i].keys[ei].key, keys[ni].key) == 0) {
                    bb_log_e(TAG,
                             "namespace %s: duplicate key '%s' (already registered)",
                             namespace, keys[ni].key);
                    return BB_ERR_INVALID_STATE;
                }
            }
        }
    }

    if (nv_reg.count >= NV_NAMESPACE_CAP) {
        bb_log_e(TAG, "nvs registry full (cap %d)", NV_NAMESPACE_CAP);
        return BB_ERR_NO_SPACE;
    }

    nv_reg.entries[nv_reg.count].namespace = namespace;
    nv_reg.entries[nv_reg.count].keys = keys;
    nv_reg.entries[nv_reg.count].n = n;
    nv_reg.count++;

    return BB_OK;
}

bb_err_t bb_manifest_register_mdns(const char *service,
                                   const bb_manifest_mdns_t *keys, size_t n)
{
    if (!service || !keys || n == 0) {
        return BB_ERR_INVALID_ARG;
    }

    // Check total key count doesn't exceed cap per service
    if (n > MDNS_KEYS_PER_SERVICE_CAP) {
        bb_log_e(TAG, "service %s: %zu keys exceeds cap %d",
                 service, n, MDNS_KEYS_PER_SERVICE_CAP);
        return BB_ERR_NO_SPACE;
    }

    // Check if service already registered
    for (size_t i = 0; i < mdns_reg.count; i++) {
        if (strcmp(mdns_reg.entries[i].service, service) == 0) {
            bb_log_e(TAG, "service %s already registered", service);
            return BB_ERR_INVALID_STATE;
        }
    }

    if (mdns_reg.count >= MDNS_SERVICE_CAP) {
        bb_log_e(TAG, "mdns registry full (cap %d)", MDNS_SERVICE_CAP);
        return BB_ERR_NO_SPACE;
    }

    mdns_reg.entries[mdns_reg.count].service = service;
    mdns_reg.entries[mdns_reg.count].keys = keys;
    mdns_reg.entries[mdns_reg.count].n = n;
    mdns_reg.count++;

    return BB_OK;
}

void bb_manifest_clear(void)
{
    nv_reg.count = 0;
    mdns_reg.count = 0;
}

// ---------------------------------------------------------------------------
// Emit
// ---------------------------------------------------------------------------

// Helper to split "|"-separated string into JSON array.
// Caller must ensure values_str != NULL.
static bb_json_t emit_values_array(const char *values_str)
{
    bb_json_t arr = bb_json_arr_new();
    if (!arr) {
        return NULL;
    }

    // Parse the pipe-separated string
    const char *p = values_str;
    while (*p) {
        // Find the extent of the current value
        const char *start = p;
        const char *end = p;
        while (*end && *end != '|') {
            end++;
        }

        // Extract the value (between start and end)
        size_t len = (size_t)(end - start);
        if (len > 0) {
            bb_json_arr_append_string_n(arr, start, len);
        }

        // Move past the pipe (if present)
        if (*end == '|') {
            p = end + 1;
        } else {
            p = end;
        }
    }

    return arr;
}

// Append all keys from a single nv_namespace_entry_t into keys_arr.
// Returns false on OOM (caller must free and return NULL).
static bool emit_nv_keys(bb_json_t keys_arr, bb_json_t ns_obj,
                         bb_json_t nvs_arr, bb_json_t root,
                         const nv_namespace_entry_t *entry)
{
    for (size_t j = 0; j < entry->n; j++) {
        const bb_manifest_nv_t *key = &entry->keys[j];

        bb_json_t key_obj = bb_json_obj_new();
        if (!key_obj) {
            bb_log_e(TAG, "failed to allocate key object");
            bb_json_free(keys_arr);
            bb_json_free(ns_obj);
            bb_json_free(nvs_arr);
            bb_json_free(root);
            return false;
        }

        bb_json_obj_set_string(key_obj, "key", key->key);
        bb_json_obj_set_string(key_obj, "type", key->type);

        // Set default (null if key->default_ is NULL, otherwise a string)
        if (key->default_ == NULL) {
            bb_json_obj_set_null(key_obj, "default");
        } else {
            bb_json_obj_set_string(key_obj, "default", key->default_);
        }

        // Set max_len (omit or set to 0 when not applicable)
        if (key->max_len > 0) {
            bb_json_obj_set_number(key_obj, "max_len", (double)key->max_len);
        }

        bb_json_obj_set_string(key_obj, "desc", key->desc);
        bb_json_obj_set_bool(key_obj, "reboot_required", key->reboot_required);
        bb_json_obj_set_bool(key_obj, "provisioning_only", key->provisioning_only);

        bb_json_arr_append_obj(keys_arr, key_obj);
    }
    return true;
}

bb_json_t bb_manifest_emit(void)
{
    bb_json_t root = bb_json_obj_new();
    if (!root) {
        bb_log_e(TAG, "failed to allocate root object");
        return NULL;
    }

    // Build NVS array, grouping all entries by namespace so multiple callers
    // contributing keys to the same namespace emit as one logical object.
    bb_json_t nvs_arr = bb_json_arr_new();
    if (!nvs_arr) {
        bb_json_free(root);
        bb_log_e(TAG, "failed to allocate nvs array");
        return NULL;
    }

    // Track which entry indices have already been emitted (first-entry wins
    // as the anchor for each namespace).
    bool emitted[NV_NAMESPACE_CAP] = {false};

    for (size_t i = 0; i < nv_reg.count; i++) {
        if (emitted[i]) {
            continue;
        }
        emitted[i] = true;

        const nv_namespace_entry_t *anchor = &nv_reg.entries[i];

        bb_json_t ns_obj = bb_json_obj_new();
        if (!ns_obj) {
            bb_json_free(nvs_arr);
            bb_json_free(root);
            bb_log_e(TAG, "failed to allocate namespace object");
            return NULL;
        }

        bb_json_obj_set_string(ns_obj, "namespace", anchor->namespace);

        // Build keys array: anchor entry first, then any later entries that
        // share the same namespace.
        bb_json_t keys_arr = bb_json_arr_new();
        if (!keys_arr) {
            bb_json_free(ns_obj);
            bb_json_free(nvs_arr);
            bb_json_free(root);
            bb_log_e(TAG, "failed to allocate keys array");
            return NULL;
        }

        // Anchor
        if (!emit_nv_keys(keys_arr, ns_obj, nvs_arr, root, anchor)) {
            return NULL;
        }

        // Additional entries with the same namespace
        for (size_t k = i + 1; k < nv_reg.count; k++) {
            if (strcmp(nv_reg.entries[k].namespace, anchor->namespace) != 0) {
                continue;
            }
            emitted[k] = true;
            if (!emit_nv_keys(keys_arr, ns_obj, nvs_arr, root,
                               &nv_reg.entries[k])) {
                return NULL;
            }
        }

        bb_json_obj_set_obj(ns_obj, "keys", keys_arr);
        bb_json_arr_append_obj(nvs_arr, ns_obj);
    }

    bb_json_obj_set_obj(root, "nvs", nvs_arr);

    // Build mDNS array
    bb_json_t mdns_arr = bb_json_arr_new();
    if (!mdns_arr) {
        bb_json_free(root);
        bb_log_e(TAG, "failed to allocate mdns array");
        return NULL;
    }

    for (size_t i = 0; i < mdns_reg.count; i++) {
        const mdns_service_entry_t *entry = &mdns_reg.entries[i];

        bb_json_t svc_obj = bb_json_obj_new();
        if (!svc_obj) {
            bb_json_free(mdns_arr);
            bb_json_free(root);
            bb_log_e(TAG, "failed to allocate service object");
            return NULL;
        }

        bb_json_obj_set_string(svc_obj, "service", entry->service);

        // Build txt array for this service
        bb_json_t txt_arr = bb_json_arr_new();
        if (!txt_arr) {
            bb_json_free(svc_obj);
            bb_json_free(mdns_arr);
            bb_json_free(root);
            bb_log_e(TAG, "failed to allocate txt array");
            return NULL;
        }

        for (size_t j = 0; j < entry->n; j++) {
            const bb_manifest_mdns_t *txt = &entry->keys[j];

            bb_json_t txt_obj = bb_json_obj_new();
            if (!txt_obj) {
                bb_json_free(txt_arr);
                bb_json_free(svc_obj);
                bb_json_free(mdns_arr);
                bb_json_free(root);
                bb_log_e(TAG, "failed to allocate txt object");
                return NULL;
            }

            bb_json_obj_set_string(txt_obj, "key", txt->key);
            bb_json_obj_set_string(txt_obj, "desc", txt->desc);

            // Set values (as JSON array if present, omit if NULL)
            if (txt->values != NULL) {
                bb_json_t values_arr = emit_values_array(txt->values);
                if (!values_arr) {
                    bb_json_free(txt_obj);
                    bb_json_free(txt_arr);
                    bb_json_free(svc_obj);
                    bb_json_free(mdns_arr);
                    bb_json_free(root);
                    bb_log_e(TAG, "failed to allocate values array");
                    return NULL;
                }
                bb_json_obj_set_obj(txt_obj, "values", values_arr);
            }

            bb_json_arr_append_obj(txt_arr, txt_obj);
        }

        bb_json_obj_set_obj(svc_obj, "txt", txt_arr);
        bb_json_arr_append_obj(mdns_arr, svc_obj);
    }

    bb_json_obj_set_obj(root, "mdns", mdns_arr);

    return root;
}
