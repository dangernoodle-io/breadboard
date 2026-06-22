// bb_mem — host backend. No SPIRAM on host; plain malloc/calloc/free.

#include "bb_mem.h"
#include <stdlib.h>

void *bb_malloc_prefer_spiram(size_t size)
{
    return malloc(size);
}

void *bb_calloc_prefer_spiram(size_t n, size_t size)
{
    return calloc(n, size);
}

void bb_mem_free(void *p)
{
    free(p);
}
