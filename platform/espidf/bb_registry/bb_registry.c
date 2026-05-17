#include "bb_registry.h"
#include "bb_http.h"
#include <stdio.h>
#include <stdlib.h>

typedef struct node {
    const bb_registry_entry_t *entry;
    struct node               *next;
} node_t;

typedef struct node_early {
    const bb_registry_entry_early_t *entry;
    struct node_early               *next;
} node_early_t;

typedef struct node_pre_http {
    const bb_registry_entry_pre_http_t *entry;
    struct node_pre_http               *next;
} node_pre_http_t;

static node_t *s_head = NULL;
static node_early_t *s_early_head = NULL;
static node_pre_http_t *s_pre_http_head = NULL;

void bb_registry_add(const bb_registry_entry_t *entry)
{
    if (!entry) return;

    node_t *n = (node_t *)malloc(sizeof(node_t));
    if (!n) return;

    n->entry = entry;
    n->next  = s_head;
    s_head   = n;
}

bb_err_t bb_registry_init(void)
{
    bb_err_t first_error = BB_OK;

    // 1. Walk PRE_HTTP entries (after EARLY, before server start)
    {
        size_t count = bb_registry_count_pre_http();
        printf("[bb_registry] pre_http init: %zu entries\n", count);

        node_pre_http_t *nodes[256];
        size_t n = 0;
        for (node_pre_http_t *p = s_pre_http_head; p && n < 256; p = p->next) {
            nodes[n++] = p;
        }

        for (int i = (int)n - 1; i >= 0; i--) {
            bb_err_t err = nodes[i]->entry->init();
            if (err != BB_OK && first_error == BB_OK) {
                first_error = err;
            }
        }
    }

    // 2. Autostart HTTP server if enabled and not already started
#if defined(CONFIG_BB_HTTP_AUTOSTART) && CONFIG_BB_HTTP_AUTOSTART
    if (!bb_http_server_get_handle()) {
        bb_err_t err = bb_http_server_start();
        if (err != BB_OK && first_error == BB_OK) {
            first_error = err;
        }
    }
#endif

    // 3. Walk regular entries (route registration — server must be up)
    {
        bb_http_handle_t server = bb_http_server_get_handle();
        size_t count = bb_registry_count();
        printf("[bb_registry] registry init: %zu entries\n", count);

        // Walk from head to tail; since we prepend, this walks in reverse order
        // To visit in insertion order, we need to reverse the list or count from tail
        // For now, follow the spec: walk in insertion order by reversing the walk
        node_t *nodes[256];
        size_t n = 0;
        for (node_t *p = s_head; p && n < 256; p = p->next) {
            nodes[n++] = p;
        }

        // Reverse into insertion order (we prepend, so head is last-registered).
        for (size_t lo = 0, hi = n - 1; lo < hi; lo++, hi--) {
            node_t *tmp = nodes[lo]; nodes[lo] = nodes[hi]; nodes[hi] = tmp;
        }
        // Stable insertion sort by .order ascending — preserves insertion-order
        // tie-breaking because the array is already in insertion order.
        for (size_t i = 1; i < n; i++) {
            node_t *key = nodes[i];
            int j = (int)i - 1;
            while (j >= 0 && nodes[j]->entry->order > key->entry->order) {
                nodes[j + 1] = nodes[j];
                j--;
            }
            nodes[j + 1] = key;
        }

        for (size_t i = 0; i < n; i++) {
            bb_err_t err = nodes[i]->entry->init(server);
            if (err != BB_OK && first_error == BB_OK) {
                first_error = err;
            }
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

size_t bb_registry_route_count_total(void)
{
    size_t total = 0;
    for (node_t *p = s_head; p; p = p->next) {
        total += (size_t)p->entry->order;
    }
    return total;
}

void bb_registry_add_early(const bb_registry_entry_early_t *entry)
{
    if (!entry) return;

    node_early_t *n = (node_early_t *)malloc(sizeof(node_early_t));
    if (!n) return;

    n->entry = entry;
    n->next  = s_early_head;
    s_early_head   = n;
}

bb_err_t bb_registry_init_early(void)
{
    size_t count = bb_registry_count_early();
    printf("[bb_registry] early init: %zu entries\n", count);

    bb_err_t first_error = BB_OK;

    // Walk from head to tail; since we prepend, this walks in reverse order
    // To visit in insertion order, we need to reverse the list or count from tail
    // For now, follow the spec: walk in insertion order by reversing the walk
    node_early_t *nodes[256];
    size_t n = 0;
    for (node_early_t *p = s_early_head; p && n < 256; p = p->next) {
        nodes[n++] = p;
    }

    // Walk backwards to get insertion order (since we prepend)
    for (int i = (int)n - 1; i >= 0; i--) {
        bb_err_t err = nodes[i]->entry->init();
        if (err != BB_OK && first_error == BB_OK) {
            first_error = err;
        }
    }

    return first_error;
}

size_t bb_registry_count_early(void)
{
    size_t count = 0;
    for (node_early_t *p = s_early_head; p; p = p->next) {
        count++;
    }
    return count;
}

void bb_registry_foreach_early(void (*cb)(const bb_registry_entry_early_t *, void *), void *ctx)
{
    if (!cb) return;

    // Collect nodes to walk in insertion order
    node_early_t *nodes[256];
    size_t n = 0;
    for (node_early_t *p = s_early_head; p && n < 256; p = p->next) {
        nodes[n++] = p;
    }

    // Walk backwards to get insertion order
    for (int i = (int)n - 1; i >= 0; i--) {
        cb(nodes[i]->entry, ctx);
    }
}

void bb_registry_clear_early(void)
{
    while (s_early_head) {
        node_early_t *tmp = s_early_head;
        s_early_head = s_early_head->next;
        free(tmp);
    }
}

void bb_registry_add_pre_http(const bb_registry_entry_pre_http_t *entry)
{
    if (!entry) return;

    node_pre_http_t *n = (node_pre_http_t *)malloc(sizeof(node_pre_http_t));
    if (!n) return;

    n->entry = entry;
    n->next  = s_pre_http_head;
    s_pre_http_head = n;
}

bb_err_t bb_registry_init_pre_http(void)
{
    size_t count = bb_registry_count_pre_http();
    printf("[bb_registry] pre_http init: %zu entries\n", count);

    bb_err_t first_error = BB_OK;

    node_pre_http_t *nodes[256];
    size_t n = 0;
    for (node_pre_http_t *p = s_pre_http_head; p && n < 256; p = p->next) {
        nodes[n++] = p;
    }

    for (int i = (int)n - 1; i >= 0; i--) {
        bb_err_t err = nodes[i]->entry->init();
        if (err != BB_OK && first_error == BB_OK) {
            first_error = err;
        }
    }

    return first_error;
}

size_t bb_registry_count_pre_http(void)
{
    size_t count = 0;
    for (node_pre_http_t *p = s_pre_http_head; p; p = p->next) {
        count++;
    }
    return count;
}

void bb_registry_foreach_pre_http(void (*cb)(const bb_registry_entry_pre_http_t *, void *), void *ctx)
{
    if (!cb) return;

    node_pre_http_t *nodes[256];
    size_t n = 0;
    for (node_pre_http_t *p = s_pre_http_head; p && n < 256; p = p->next) {
        nodes[n++] = p;
    }

    for (int i = (int)n - 1; i >= 0; i--) {
        cb(nodes[i]->entry, ctx);
    }
}

void bb_registry_clear_pre_http(void)
{
    while (s_pre_http_head) {
        node_pre_http_t *tmp = s_pre_http_head;
        s_pre_http_head = s_pre_http_head->next;
        free(tmp);
    }
}
