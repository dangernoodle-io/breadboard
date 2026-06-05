#include "bb_http.h"

// GET /api/board was removed — superseded by GET /api/info which includes
// the same board fields plus heap and network info. No routes registered here;
// the registry entry was removed because it registers zero routes and the
// sort-key-only order value contributes no useful information.
