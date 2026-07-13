#include "bb_cache_routes.h"

// Host twin of the bb_cache_routes component. The handler itself is
// httpd-backed (platform/espidf/bb_cache_routes/bb_cache_routes.c) and is not
// compiled on host -- only the pure mapper (src/cache_route_status.c) is
// host-testable. This file exists solely to provide the test-reset hook the
// native harness expects every *_TESTING component to expose.

#ifdef BB_CACHE_ROUTES_TESTING
void bb_cache_routes_reset_for_test(void)
{
    // bb_cache_routes holds no persistent state to reset -- the mapper is a
    // pure function and the handler is not compiled on host. Kept for API
    // symmetry with other components' test-reset hooks (e.g.
    // bb_thermal_reset_for_test).
}
#endif
