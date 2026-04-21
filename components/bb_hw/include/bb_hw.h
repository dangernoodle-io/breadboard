#pragma once

#if defined(FIRMWARE_BOARD_elecrow_p4_hmi7)
#  include "boards/elecrow_p4_hmi7.h"
#else
#  error "Unknown board — add -DFIRMWARE_BOARD_xxx to build_flags"
#endif
