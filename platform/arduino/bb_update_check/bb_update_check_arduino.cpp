// Arduino stub for bb_update_check. The component compiles cleanly so smoke
// builds link, but every setter returns BB_ERR_UNSUPPORTED. AVR / Cortex-M
// targets in this workspace have no general HTTPS client and the polling
// model doesn't suit a cooperative `loop()`.
#include "bb_update_check.h"

bb_err_t bb_update_check_init(const bb_update_check_cfg_t *cfg)
{
    (void)cfg;
    return BB_ERR_UNSUPPORTED;
}

bb_err_t bb_update_check_set_releases_url(const char *url)
{
    (void)url;
    return BB_ERR_UNSUPPORTED;
}

bb_err_t bb_update_check_set_parser(bb_release_manifest_parse_fn fn)
{
    (void)fn;
    return BB_ERR_UNSUPPORTED;
}

bb_err_t bb_update_check_set_firmware_board(const char *board)
{
    (void)board;
    return BB_ERR_UNSUPPORTED;
}

bb_err_t bb_update_check_set_hooks(bb_update_check_pause_cb_t pause,
                                   bb_update_check_resume_cb_t resume)
{
    (void)pause; (void)resume;
    return BB_ERR_UNSUPPORTED;
}

bb_err_t bb_update_check_now(void)
{
    return BB_ERR_UNSUPPORTED;
}

bb_err_t bb_update_check_get_status(bb_update_check_status_t *out)
{
    (void)out;
    return BB_ERR_UNSUPPORTED;
}
