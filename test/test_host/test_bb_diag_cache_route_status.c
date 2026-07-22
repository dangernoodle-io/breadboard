#include "unity.h"
#include "bb_diag_cache_route_status.h"

// B1-1121: pure bb_err_t -> HTTP status mapper for the GET /api/diag/cache
// handler, folded in from the deleted bb_cache_routes component (originally
// B1-583, extracted from the ESP-IDF-only handler so Coveralls sees every
// branch of the status mapping). Six cases (each is a distinct bb_err_t
// input covering every branch of the mapper's if-chain):
//   - BB_OK               -> 200
//   - BB_ERR_NOT_FOUND     -> 404
//   - BB_ERR_INVALID_STATE -> 404
//   - BB_ERR_UNSUPPORTED   -> 501 (newly reachable, B1-1053 PR1's bb_cache
//     relaxation -- a key registered with cfg->serialize == NULL)
//   - BB_ERR_NO_SPACE      -> 500
//   - any other bb_err_t   -> 500

void test_bb_diag_cache_route_map_status_ok_is_200(void)
{
    TEST_ASSERT_EQUAL(200, bb_diag_cache_route_map_status(BB_OK));
}

void test_bb_diag_cache_route_map_status_not_found_is_404(void)
{
    TEST_ASSERT_EQUAL(404, bb_diag_cache_route_map_status(BB_ERR_NOT_FOUND));
}

void test_bb_diag_cache_route_map_status_invalid_state_is_404(void)
{
    TEST_ASSERT_EQUAL(404, bb_diag_cache_route_map_status(BB_ERR_INVALID_STATE));
}

void test_bb_diag_cache_route_map_status_unsupported_is_501(void)
{
    TEST_ASSERT_EQUAL(501, bb_diag_cache_route_map_status(BB_ERR_UNSUPPORTED));
}

void test_bb_diag_cache_route_map_status_no_space_is_500(void)
{
    TEST_ASSERT_EQUAL(500, bb_diag_cache_route_map_status(BB_ERR_NO_SPACE));
}

void test_bb_diag_cache_route_map_status_other_is_500(void)
{
    TEST_ASSERT_EQUAL(500, bb_diag_cache_route_map_status(BB_ERR_INVALID_ARG));
}
