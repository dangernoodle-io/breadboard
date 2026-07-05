// bb_sink_display_init()'s caps-independent config validation -- pure, no
// bb_cache/bb_cache_reactive/bb_timer/bb_display calls. Shared verbatim
// between host tests and the espidf glue
// (platform/espidf/bb_sink_display/bb_sink_display.c).

#include "bb_sink_display.h"

bb_err_t bb_sink_display_validate_config(const bb_sink_display_caps_t *caps,
                                          const bb_sink_display_config_t *cfg)
{
    if (!caps || !cfg) return BB_ERR_INVALID_ARG;
    if (cfg->kind == BB_SINK_DISPLAY_POLICY_CUSTOM && !cfg->custom) return BB_ERR_INVALID_ARG;
    if (cfg->evict_after_ms <= cfg->stale_after_ms) return BB_ERR_INVALID_ARG;
    if (cfg->display != NULL) {
        // Reserved multi-display seam -- see bb_sink_display_config_t.display.
        return BB_ERR_UNSUPPORTED;
    }
    return BB_OK;
}
