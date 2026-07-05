#include "unity.h"
#include "cache_route_status.h"

// B1-583: pure bb_err_t -> HTTP status mapper for the GET /api/cache handler,
// extracted from the ESP-IDF-only handler in bb_cache_routes.c so Coveralls
// sees every branch of the status mapping. Five cases (each is a distinct
// bb_err_t input covering every branch of the mapper's if-chain):
//   - BB_OK               -> 200
//   - BB_ERR_NOT_FOUND     -> 404
//   - BB_ERR_INVALID_STATE -> 404
//   - BB_ERR_NO_SPACE      -> 500
//   - any other bb_err_t   -> 500

void test_cache_route_map_status_ok_is_200(void)
{
    TEST_ASSERT_EQUAL(200, cache_route_map_status(BB_OK));
}

void test_cache_route_map_status_not_found_is_404(void)
{
    TEST_ASSERT_EQUAL(404, cache_route_map_status(BB_ERR_NOT_FOUND));
}

void test_cache_route_map_status_invalid_state_is_404(void)
{
    TEST_ASSERT_EQUAL(404, cache_route_map_status(BB_ERR_INVALID_STATE));
}

void test_cache_route_map_status_no_space_is_500(void)
{
    TEST_ASSERT_EQUAL(500, cache_route_map_status(BB_ERR_NO_SPACE));
}

void test_cache_route_map_status_other_is_500(void)
{
    TEST_ASSERT_EQUAL(500, cache_route_map_status(BB_ERR_INVALID_ARG));
}
