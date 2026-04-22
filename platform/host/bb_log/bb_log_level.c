#include "bb_log.h"

void _bb_log_level_set_backend(const char *tag, bb_log_level_t level)
{
    (void)tag;
    (void)level;
    // Host backend: no-op
}
