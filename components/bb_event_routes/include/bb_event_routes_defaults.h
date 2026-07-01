// bb_event_routes — shared Kconfig host-fallback defaults.
//
// Single point of truth for the host-build fallback of
// CONFIG_BB_EVENT_ROUTES_MAX_CLIENTS and CONFIG_BB_EVENT_ROUTES_QUEUE_DEPTH,
// included by bb_event_routes_common.c and test_route_fidelity.c. On
// ESP-IDF the build system always supplies these via sdkconfig.h, so this
// fallback only takes effect on host builds without a Kconfig.
//
// Must match components/bb_event_routes/Kconfig.
#pragma once

#ifndef CONFIG_BB_EVENT_ROUTES_MAX_CLIENTS
#define CONFIG_BB_EVENT_ROUTES_MAX_CLIENTS 2
#endif
#ifndef CONFIG_BB_EVENT_ROUTES_QUEUE_DEPTH
#define CONFIG_BB_EVENT_ROUTES_QUEUE_DEPTH 8
#endif
