#pragma once

// Consumer-supplied board header. Set -DBB_HW_BOARD_HEADER="<path>.h"
// and provide the header on the include path.
#ifndef BB_HW_BOARD_HEADER
#  error "bb_hw: define BB_HW_BOARD_HEADER (-DBB_HW_BOARD_HEADER=\"<header>.h\")"
#endif
#include BB_HW_BOARD_HEADER
