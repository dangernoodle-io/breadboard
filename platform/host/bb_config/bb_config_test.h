#pragma once

#include "bb_config.h"
#include "bb_storage.h"

/**
 * Host-only test hook: exposes bb_config.c's internal (static)
 * bb_config_type_t -> bb_storage_enc_t mapper for direct, pure unit testing
 * without going through a full get/set round trip. Only available when
 * BB_CONFIG_TESTING is defined.
 */
#ifdef BB_CONFIG_TESTING
bb_storage_enc_t bb_config_cfg_type_to_enc_for_test(bb_config_type_t t);
#endif
