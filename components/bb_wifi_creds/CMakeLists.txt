idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../platform/host/bb_wifi_creds/bb_wifi_creds.c"
    INCLUDE_DIRS "include"
    REQUIRES bb_core
)

# bb_wifi_creds_read is a pure provider-vs-fallback dispatcher (no ESP-IDF
# deps) -- compiles unchanged on host and ESP-IDF, filed under platform/host/
# only, mirroring bb_settings and bb_dispatch_cmd.
# bbtool-scaffold-hint: source=platform/host/bb_wifi_creds/bb_wifi_creds.c
