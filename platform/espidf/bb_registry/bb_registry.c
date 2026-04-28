#include "bb_registry.h"
#include "bb_log.h"
#include <stdlib.h>

static const char *TAG = "bb_registry";

typedef struct node {
    const bb_registry_entry_t *entry;
    struct node               *next;
} node_t;

static node_t *s_head = NULL;

void bb_registry_add(const bb_registry_entry_t *entry)
{
    if (!entry) return;

    node_t *n = (node_t *)malloc(sizeof(node_t));
    if (!n) return;

    n->entry = entry;
    n->next  = s_head;
    s_head   = n;
}

bb_err_t bb_registry_init(bb_http_handle_t server)
{
    size_t count = bb_registry_count();
    bb_log_i(TAG, "registry init: %u entries", count);

    bb_err_t first_error = BB_OK;

    // Walk from head to tail; since we prepend, this walks in reverse order
    // To visit in insertion order, we need to reverse the list or count from tail
    // For now, follow the spec: walk in insertion order by reversing the walk
    node_t *nodes[256];
    size_t n = 0;
    for (node_t *p = s_head; p && n < 256; p = p->next) {
        nodes[n++] = p;
    }

    // Walk backwards to get insertion order (since we prepend)
    for (int i = (int)n - 1; i >= 0; i--) {
        bb_err_t err = nodes[i]->entry->init(server);
        if (err != BB_OK && first_error == BB_OK) {
            first_error = err;
        }
    }

    return first_error;
}

size_t bb_registry_count(void)
{
    size_t count = 0;
    for (node_t *p = s_head; p; p = p->next) {
        count++;
    }
    return count;
}

void bb_registry_foreach(void (*cb)(const bb_registry_entry_t *, void *), void *ctx)
{
    if (!cb) return;

    // Collect nodes to walk in insertion order
    node_t *nodes[256];
    size_t n = 0;
    for (node_t *p = s_head; p && n < 256; p = p->next) {
        nodes[n++] = p;
    }

    // Walk backwards to get insertion order
    for (int i = (int)n - 1; i >= 0; i--) {
        cb(nodes[i]->entry, ctx);
    }
}

void bb_registry_clear(void)
{
    while (s_head) {
        node_t *tmp = s_head;
        s_head = s_head->next;
        free(tmp);
    }
}
