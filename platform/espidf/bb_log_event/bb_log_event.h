#pragma once

// bb_log_event — ESP-IDF only: "log" bb_event stream topic. No
// components/bb_log_event/include/ dir exists; this flat header is the
// codegen-visible surface (wire.py falls back to platform/espidf/<name>/*.h
// for components without a component-local include/).

#ifdef ESP_PLATFORM

#include "bb_core.h"
#include "bb_http_server.h"

// bbtool:init tier=regular fn=bb_log_event_init server=true
bb_err_t bb_log_event_init(bb_http_handle_t server);

#endif /* ESP_PLATFORM */
