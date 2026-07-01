// bb_pub — shared Kconfig host-fallback defaults.
//
// Single point of truth for the host-build fallback of
// CONFIG_BB_PUB_TELEM_SNAP_MAX, included by bb_pub core and every telemetry
// satellite (bb_pub_fan, bb_pub_info, bb_pub_power, bb_pub_telemetry,
// bb_pub_thermal, bb_pub_wifi) that sizes a static snapshot buffer against
// it. On ESP-IDF the build system always supplies CONFIG_BB_PUB_TELEM_SNAP_MAX
// via sdkconfig.h, so this fallback only takes effect on host builds without
// a Kconfig.
//
// Must match the no-PSRAM default in components/bb_pub/Kconfig. Raised from
// 256 to 512 to fit the meta snapshot's device-identity strings.
#pragma once

#ifndef CONFIG_BB_PUB_TELEM_SNAP_MAX
#define CONFIG_BB_PUB_TELEM_SNAP_MAX 512
#endif
