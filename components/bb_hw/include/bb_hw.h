#pragma once

// Consumer-supplied board header:
//   Set -DBB_HW_BOARD_HEADER="boards/<name>.h" and provide <name>.h on the
//   include path via your own component's INCLUDE_DIRS. Falls back to bundled
//   headers under components/bb_hw/include/boards/ when no override is given.
#ifdef BB_HW_BOARD_HEADER
#  include BB_HW_BOARD_HEADER
#else
#  if defined(FIRMWARE_BOARD_elecrow_p4_hmi7)
#    include "boards/elecrow_p4_hmi7.h"
#  elif defined(FIRMWARE_BOARD_esp32_wroom_32)
#    include "boards/esp32_wroom_32.h"
#  else
#    error "bb_hw: define BB_HW_BOARD_HEADER or FIRMWARE_BOARD_*"
#  endif
#endif
