#include "bb_log_config.h"
#include "bb_kv.h"

#include <string.h>

// Kconfig -> C bridge. C default matches the Kconfig default (mandatory
// pattern — never shadow the generated CONFIG_ symbol with a bare #ifndef).
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#ifdef CONFIG_BB_LOG_DEFAULT_LEVEL
#define BB_LOG_DEFAULT_LEVEL_STR CONFIG_BB_LOG_DEFAULT_LEVEL
#endif
#ifdef CONFIG_BB_LOG_LEVELS
#define BB_LOG_LEVELS_STR CONFIG_BB_LOG_LEVELS
#endif
#endif

#ifndef BB_LOG_DEFAULT_LEVEL_STR
#define BB_LOG_DEFAULT_LEVEL_STR "info"
#endif

#ifndef BB_LOG_LEVELS_STR
#define BB_LOG_LEVELS_STR ""
#endif

static const char *TAG = "bb_log_config";

bool bb_log_level_from_name(const char *name, size_t len, bb_log_level_t *out)
{
    if (!name || !out || len == 0 || len >= 16) return false;

    char buf[16];
    memcpy(buf, name, len);
    buf[len] = '\0';

    return bb_log_level_from_str(buf, out);
}

void bb_log_config_apply_kv(const char *key, size_t key_len,
                            const char *val, size_t val_len, void *ctx)
{
    (void)ctx;

    char tag_buf[32];
    size_t copy_len = key_len < sizeof(tag_buf) - 1 ? key_len : sizeof(tag_buf) - 1;
    memcpy(tag_buf, key, copy_len);
    tag_buf[copy_len] = '\0';

    if (val_len == 0) {
        bb_log_w(TAG, "empty log level for tag '%s', skipping", tag_buf);
        return;
    }

    bb_log_level_t level;
    if (!bb_log_level_from_name(val, val_len, &level)) {
        bb_log_w(TAG, "unknown log level for tag '%s', skipping", tag_buf);
        return;
    }

    bb_log_level_set(tag_buf, level);
}

void bb_log_config_apply_levels(const char *s)
{
    bb_kv_parse(s, bb_log_config_apply_kv, NULL);
}

bb_err_t bb_log_config_apply(const char *default_level_str, const char *levels_str)
{
    bb_log_level_t level;
    if (bb_log_level_from_str(default_level_str, &level)) {
        bb_log_level_set("*", level);
    } else {
        bb_log_w(TAG, "invalid default log level '%s', leaving platform default",
                 default_level_str ? default_level_str : "(null)");
    }

    bb_log_config_apply_levels(levels_str);

    return BB_OK;
}

bb_err_t bb_log_config_init(void)
{
    return bb_log_config_apply(BB_LOG_DEFAULT_LEVEL_STR, BB_LOG_LEVELS_STR);
}
