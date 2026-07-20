#include "bb_http_section_status.h"

int bb_http_section_status_for_render(bb_err_t rc)
{
    if (rc == BB_OK) return 200;
    if (rc == BB_ERR_NOT_FOUND) return 404;
    return 500;
}

int bb_http_section_status_for_apply(bb_http_section_apply_result_t result,
                                      int unsupported_status_override)
{
    if (result.stage == BB_HTTP_SECTION_STAGE_PARSE) {
        if (result.rc == BB_ERR_PARSE_GRAMMAR || result.rc == BB_ERR_PARSE_INCOMPLETE) return 400;
        if (result.rc == BB_ERR_NOT_FOUND) return 404;
        if (result.rc == BB_ERR_UNSUPPORTED) return 405;
        return 500;
    }

    // BB_HTTP_SECTION_STAGE_COMMIT
    if (result.rc == BB_OK) return 200;
    if (result.rc == BB_ERR_VALIDATION) return 400;
    if (result.rc == BB_ERR_UNSUPPORTED) {
        return unsupported_status_override != 0 ? unsupported_status_override : 405;
    }
    return 500;
}
