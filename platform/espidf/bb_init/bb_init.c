#include "bb_init.h"
#include "bb_mem.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct node {
    const bb_init_entry_t *entry;
    struct node               *next;
} node_t;

typedef struct node_early {
    const bb_init_entry_early_t *entry;
    struct node_early               *next;
} node_early_t;

typedef struct node_pre_http {
    const bb_init_entry_pre_http_t *entry;
    struct node_pre_http               *next;
} node_pre_http_t;

static node_t *s_head = NULL;
static node_early_t *s_early_head = NULL;
static node_pre_http_t *s_pre_http_head = NULL;
static bool s_pre_http_walked = false;

typedef int (*bb_init_order_getter_fn)(const void *node);

// Reverses a head-is-most-recently-registered node array into insertion order,
// then runs a stable insertion sort ascending by .order (ties preserve
// insertion order because the array is already in insertion order before the
// sort begins). No-ops for n < 2 — this is what guards the n==0 case that
// previously underflowed `hi = n - 1` to SIZE_MAX.
static void bb_init_sort_nodes_by_order(void **nodes, size_t n, bb_init_order_getter_fn get_order)
{
    if (n < 2) return;

    for (size_t lo = 0, hi = n - 1; lo < hi; lo++, hi--) {
        void *tmp = nodes[lo]; nodes[lo] = nodes[hi]; nodes[hi] = tmp;
    }

    for (size_t i = 1; i < n; i++) {
        void *key = nodes[i];
        int key_order = get_order(key);
        int j = (int)i - 1;
        while (j >= 0 && get_order(nodes[j]) > key_order) {
            nodes[j + 1] = nodes[j];
            j--;
        }
        nodes[j + 1] = key;
    }
}

static int bb_init_order_regular(const void *node)
{
    return ((const node_t *)node)->entry->order;
}

static int bb_init_order_early(const void *node)
{
    return ((const node_early_t *)node)->entry->order;
}

static int bb_init_order_pre_http(const void *node)
{
    return ((const node_pre_http_t *)node)->entry->order;
}

void bb_init_add(const bb_init_entry_t *entry)
{
    if (!entry) return;

    node_t *n = (node_t *)bb_malloc_prefer_spiram(sizeof(node_t));
    if (!n) return;

    n->entry = entry;
    n->next  = s_head;
    s_head   = n;
}

bb_err_t bb_init_init(void)
{
    bb_err_t first_error = BB_OK;

    // 1. Walk PRE_HTTP entries (after EARLY). bb_init has no knowledge of
    // bb_http_server whatsoever (KB #692 decoupling) — if a firmware needs the HTTP
    // server up before REGULAR-tier init runs, bb_http_server self-registers its own
    // PRE_HTTP-tier autostart hook ordered last (see platform/espidf/bb_http_server/bb_http.c). Delegates to
    // the standalone entry point so the walked-once guard is shared.
    {
        bb_err_t err = bb_init_init_pre_http();
        // No `&& first_error == BB_OK` guard here (unlike the regular-tier
        // loop below): first_error was just initialized to BB_OK a few lines
        // up and nothing else can have run before this single call, so that
        // second operand is a tautology at this call site.
        if (err != BB_OK) {
            first_error = err;
        }
    }

    // 2. Walk regular entries. Legacy BB_INIT_REGISTER entries take a
    // bb_http_handle_t server argument for source compatibility, but the
    // value is resolved by a per-component trampoline living in the
    // CONSUMER's translation unit (see BB_INIT_REGISTER_N in bb_init.h) — not
    // here. bb_init never calls into bb_http_server, so it passes NULL.
    {
        size_t count = bb_init_count();
        printf("[bb_init] registry init: %zu entries\n", count);

        void *nodes[256];
        size_t n = 0;
        for (node_t *p = s_head; p && n < 256; p = p->next) {
            nodes[n++] = p;
        }

        bb_init_sort_nodes_by_order(nodes, n, bb_init_order_regular);

        for (size_t i = 0; i < n; i++) {
            node_t *nd = (node_t *)nodes[i];
            bb_err_t err = nd->entry->init(NULL);
            if (err != BB_OK && first_error == BB_OK) {
                first_error = err;
            }
        }
    }

    return first_error;
}

size_t bb_init_count(void)
{
    size_t count = 0;
    for (node_t *p = s_head; p; p = p->next) {
        count++;
    }
    return count;
}

void bb_init_foreach(void (*cb)(const bb_init_entry_t *, void *), void *ctx)
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

void bb_init_clear(void)
{
    while (s_head) {
        node_t *tmp = s_head;
        s_head = s_head->next;
        bb_mem_free(tmp);
    }
}

void bb_init_add_early(const bb_init_entry_early_t *entry)
{
    if (!entry) return;

    node_early_t *n = (node_early_t *)bb_malloc_prefer_spiram(sizeof(node_early_t));
    if (!n) return;

    n->entry = entry;
    n->next  = s_early_head;
    s_early_head   = n;
}

bb_err_t bb_init_init_early(void)
{
    size_t count = bb_init_count_early();
    printf("[bb_init] early init: %zu entries\n", count);

    bb_err_t first_error = BB_OK;

    void *nodes[256];
    size_t n = 0;
    for (node_early_t *p = s_early_head; p && n < 256; p = p->next) {
        nodes[n++] = p;
    }

    bb_init_sort_nodes_by_order(nodes, n, bb_init_order_early);

    for (size_t i = 0; i < n; i++) {
        node_early_t *nd = (node_early_t *)nodes[i];
        bb_err_t err = nd->entry->init();
        if (err != BB_OK && first_error == BB_OK) {
            first_error = err;
        }
    }

    return first_error;
}

size_t bb_init_count_early(void)
{
    size_t count = 0;
    for (node_early_t *p = s_early_head; p; p = p->next) {
        count++;
    }
    return count;
}

void bb_init_foreach_early(void (*cb)(const bb_init_entry_early_t *, void *), void *ctx)
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

void bb_init_clear_early(void)
{
    while (s_early_head) {
        node_early_t *tmp = s_early_head;
        s_early_head = s_early_head->next;
        bb_mem_free(tmp);
    }
}

void bb_init_add_pre_http(const bb_init_entry_pre_http_t *entry)
{
    if (!entry) return;

    node_pre_http_t *n = (node_pre_http_t *)bb_malloc_prefer_spiram(sizeof(node_pre_http_t));
    if (!n) return;

    n->entry = entry;
    n->next  = s_pre_http_head;
    s_pre_http_head = n;
}

bb_err_t bb_init_init_pre_http(void)
{
    if (s_pre_http_walked) {
        printf("[bb_init] pre_http init: already walked, skipping\n");
        return BB_OK;
    }

    size_t count = bb_init_count_pre_http();
    printf("[bb_init] pre_http init: %zu entries\n", count);

    bb_err_t first_error = BB_OK;

    void *nodes[256];
    size_t n = 0;
    for (node_pre_http_t *p = s_pre_http_head; p && n < 256; p = p->next) {
        nodes[n++] = p;
    }

    bb_init_sort_nodes_by_order(nodes, n, bb_init_order_pre_http);

    for (size_t i = 0; i < n; i++) {
        node_pre_http_t *nd = (node_pre_http_t *)nodes[i];
        bb_err_t err = nd->entry->init();
        if (err != BB_OK && first_error == BB_OK) {
            first_error = err;
        }
    }

    s_pre_http_walked = true;
    return first_error;
}

size_t bb_init_count_pre_http(void)
{
    size_t count = 0;
    for (node_pre_http_t *p = s_pre_http_head; p; p = p->next) {
        count++;
    }
    return count;
}

void bb_init_foreach_pre_http(void (*cb)(const bb_init_entry_pre_http_t *, void *), void *ctx)
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

void bb_init_clear_pre_http(void)
{
    while (s_pre_http_head) {
        node_pre_http_t *tmp = s_pre_http_head;
        s_pre_http_head = s_pre_http_head->next;
        bb_mem_free(tmp);
    }
    s_pre_http_walked = false;
}
