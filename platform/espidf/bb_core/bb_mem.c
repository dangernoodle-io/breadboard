// bb_mem — ESP-IDF SPIRAM-preferred allocation helpers.
// Try the SPIRAM/8-bit heap first; fall back to the default (internal) heap so
// boards without PSRAM keep their original behaviour. Frees via heap_caps_free.

#include "bb_mem.h"
#include "esp_heap_caps.h"

void *bb_malloc_prefer_spiram(size_t size)
{
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) {
        p = heap_caps_malloc(size, MALLOC_CAP_DEFAULT);
    }
    return p;
}

void *bb_calloc_prefer_spiram(size_t n, size_t size)
{
    void *p = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) {
        p = heap_caps_calloc(n, size, MALLOC_CAP_DEFAULT);
    }
    return p;
}

void bb_mem_free(void *p)
{
    heap_caps_free(p);
}
