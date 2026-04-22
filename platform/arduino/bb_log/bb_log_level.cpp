#include "bb_log.h"

extern "C" void _bb_log_level_set_backend(const char *tag, bb_log_level_t level)
{
    (void)tag;
    (void)level;
    // Arduino backend: no-op
}
