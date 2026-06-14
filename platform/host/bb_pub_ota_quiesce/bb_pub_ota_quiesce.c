// bb_pub_ota_quiesce — pause telemetry during update-check/OTA TLS handshake.
// Compiled on both host (tests) and ESP-IDF.
#include "bb_pub_ota_quiesce.h"
#include "bb_pub.h"
#include "bb_update_check.h"
#include "bb_ota_hooks.h"
#include "bb_log.h"
#include "bb_registry.h"

#ifndef CONFIG_BB_PUB_QUIESCE_ON_OTA
#define CONFIG_BB_PUB_QUIESCE_ON_OTA 0
#endif

static const char *TAG = "bb_pub_ota_quiesce";

// ---------------------------------------------------------------------------
// Thin wrappers — bb_http_pause_cb_t is bool(*)(void); bb_pub_pause is void.
// ---------------------------------------------------------------------------

static bool quiesce_pause(void)
{
    bb_pub_pause();
    return true;
}

static void quiesce_resume(void)
{
    bb_pub_resume();
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------

bb_err_t bb_pub_ota_quiesce_init(void)
{
    bb_err_t err = bb_update_check_set_hooks(quiesce_pause, quiesce_resume);
    if (err != BB_OK) {
        bb_log_w(TAG, "bb_update_check_set_hooks failed: %d", err);
        return err;
    }

    bb_ota_set_hooks(quiesce_pause, quiesce_resume);

    bb_log_i(TAG, "telemetry quiesce hooks installed");
    return BB_OK;
}

// ---------------------------------------------------------------------------
// Auto-attach (PRE_HTTP tier)
// ---------------------------------------------------------------------------

static bb_err_t bb_pub_ota_quiesce_pre_http_init(void)
{
    return bb_pub_ota_quiesce_init();
}

#if CONFIG_BB_PUB_QUIESCE_ON_OTA
BB_REGISTRY_REGISTER_PRE_HTTP(bb_pub_ota_quiesce, bb_pub_ota_quiesce_pre_http_init);
#endif
