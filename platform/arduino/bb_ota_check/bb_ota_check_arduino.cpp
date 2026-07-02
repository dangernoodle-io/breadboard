// Arduino stub for bb_ota_check. The component compiles cleanly so smoke
// builds link, but every setter returns BB_ERR_UNSUPPORTED. AVR / Cortex-M
// targets in this workspace have no general HTTPS client and the polling
// model doesn't suit a cooperative `loop()`.
#include "bb_ota_check.h"

bb_err_t bb_ota_check_init(const bb_ota_check_cfg_t *cfg)
{
    (void)cfg;
    return BB_ERR_UNSUPPORTED;
}

bb_err_t bb_ota_check_set_releases_url(const char *url)
{
    (void)url;
    return BB_ERR_UNSUPPORTED;
}

bb_err_t bb_ota_check_set_parser(bb_release_manifest_parse_fn fn)
{
    (void)fn;
    return BB_ERR_UNSUPPORTED;
}

bb_err_t bb_ota_check_set_firmware_board(const char *board)
{
    (void)board;
    return BB_ERR_UNSUPPORTED;
}

bb_err_t bb_ota_check_set_hooks(bb_ota_check_pause_cb_t pause,
                                   bb_ota_check_resume_cb_t resume)
{
    (void)pause; (void)resume;
    return BB_ERR_UNSUPPORTED;
}

bb_err_t bb_ota_check_now(void)
{
    return BB_ERR_UNSUPPORTED;
}

bb_err_t bb_ota_check_kick(void)
{
    // Arduino has no worker task, so provide a synchronous stub that calls now().
    // On host, both functions have the same semantics (synchronous).
    return bb_ota_check_now();
}

bb_err_t bb_ota_check_get_status(bb_ota_check_status_t *out)
{
    (void)out;
    return BB_ERR_UNSUPPORTED;
}
